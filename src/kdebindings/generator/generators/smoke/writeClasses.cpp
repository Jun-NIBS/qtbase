/*
    Generator for the SMOKE sources
    Copyright (C) 2009 Arno Rehn <arno@arnorehn.de>

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with this program; if not, write to the Free Software Foundation, Inc.,
    51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QMap>
#include <QSet>
#include <QTextStream>

#include <type.h>

#include "globals.h"
#include "../../options.h"

SmokeClassFiles::SmokeClassFiles(SmokeDataFile *data)
    : m_smokeData(data)
{
}

void SmokeClassFiles::write()
{
    write(m_smokeData->includedClasses);
}

void SmokeClassFiles::write(const QList<QString>& keys)
{
    qDebug("writing out x_*.cpp [%s]", qPrintable(Options::module));
    
    // how many classes go in one file
    int count = keys.count() / Options::parts;
    int count2 = count;
    
    for (int i = 0; i < Options::parts; i++) {
        QSet<QString> includes;
        QString classCode;
        QTextStream classOut(&classCode);
        
        // write the class code to a QString so we can later prepend the #includes
        if (i == Options::parts - 1) count2 = -1;
        foreach (const QString& str, keys.mid(count * i, count2)) {
            const Class* klass = &classes[str];
            includes.insert(klass->fileName());
            writeClass(classOut, klass, str, includes);
        }
        
        // create the file
        QFile file(Options::outputDir.filePath("x_" + QString::number(i + 1) + ".cpp"));
        file.open(QFile::ReadWrite | QFile::Truncate);

        QTextStream fileOut(&file);
        
        // write out the header
        fileOut << "//Auto-generated by " << QCoreApplication::arguments()[0] << ". DO NOT EDIT.\n";

        // ... and the #includes
        QList<QString> sortedIncludes = includes.toList();
        qSort(sortedIncludes.begin(), sortedIncludes.end());
        foreach (const QString& str, sortedIncludes) {
            if (str.isEmpty())
                continue;
            fileOut << "#include <" << str << ">\n";
        }

        fileOut << "\n#include <smoke.h>\n#include <" << Options::module << "_smoke.h>\n";

        fileOut << "\nclass __internal_SmokeClass {};\n";

        fileOut << "\nnamespace __smoke" << Options::module << " {\n\n";

        // now the class code
        fileOut << classCode;
        
        fileOut << "\n}\n";
        
        file.close();
    }
}

QString SmokeClassFiles::generateMethodBody(const QString& indent, const QString& className, const QString& smokeClassName, const Method& meth,
                                            int index, bool dynamicDispatch, QSet<QString>& includes)
{
    QString methodBody;
    QTextStream out(&methodBody);

    out << indent;

    if (meth.isConstructor()) {
        out << smokeClassName << "* xret = new " << smokeClassName << "(";
    } else {
        const Function* func = Util::globalFunctionMap[&meth];
        if (func)
            includes.insert(func->fileName());

        if (meth.type()->getClass())
            includes.insert(meth.type()->getClass()->fileName());

        if (meth.type()->isFunctionPointer() || meth.type()->isArray())
            out << meth.type()->toString("xret") << " = ";
        else if (meth.type() != Type::Void)
            out << meth.type()->toString() << " xret = ";

        if (!(meth.flags() & Method::Static)) {
            if (meth.isConst()) {
                out << "((const " << smokeClassName << "*)this)->";
            } else {
                out << "this->";
            }
        }
        if (!dynamicDispatch && !func) {
            // dynamic dispatch not wanted, call with 'this->Foo::method()'
            out << className << "::";
        } else if (func) {
            if (!func->nameSpace().isEmpty())
                out << func->nameSpace() << "::";
        }
        out << meth.name() << "(";
    }

    for (int j = 0; j < meth.parameters().count(); j++) {
        const Parameter& param = meth.parameters()[j];

        if (param.type()->getClass())
            includes.insert(param.type()->getClass()->fileName());

        if (j > 0) out << ",";

        QString field = Util::stackItemField(param.type());
        QString typeName = param.type()->toString();
        if (param.type()->isArray()) {
            Type t = *param.type();
            t.setPointerDepth(t.pointerDepth() + 1);
            t.setIsRef(false);
            typeName = t.toString();
            out << '*';
        } else if (field == "s_class" && (param.type()->pointerDepth() == 0 || param.type()->isRef()) && !param.type()->isFunctionPointer()) {
            // references and classes are passed in s_class
            typeName.append('*');
            out << '*';
        }
        // casting to a reference doesn't make sense in this case
        if (param.type()->isRef() && !param.type()->isFunctionPointer()) typeName.replace('&', "");
        out << "(" << typeName << ")" << "x[" << j + 1 << "]." << field;
    }

    // if the method has any other default parameters, append them here as values
    if (!meth.remainingDefaultValues().isEmpty()) {
        const QStringList& defaultParams = meth.remainingDefaultValues();
        if (meth.parameters().count() > 0)
            out << "," ;
        out << defaultParams.join(",");
    }

    out << ");\n";
    if (meth.type() != Type::Void) {
        out << indent << "x[0]." << Util::stackItemField(meth.type()) << " = " << Util::assignmentString(meth.type(), "xret") << ";\n";
    } else {
        out << indent << "(void)x; // noop (for compiler warning)\n";
    }

    return methodBody;
}

void SmokeClassFiles::generateMethod(QTextStream& out, const QString& className, const QString& smokeClassName,
                                     const Method& meth, int index, QSet<QString>& includes)
{
    out << "    ";
    if ((meth.flags() & Method::Static) || meth.isConstructor())
        out << "static ";
    out << QString("void x_%1(Smoke::Stack x) {\n").arg(index);
    out << "        // " << meth.toString() << "\n";

    bool dynamicDispatch = ((meth.flags() & Method::PureVirtual) || (meth.flags() & Method::DynamicDispatch));

    if (dynamicDispatch || !Util::virtualMethodsForClass(meth.getClass()).contains(&meth)) {
        // This is either already flagged as dynamic dispatch or just a normal method. We can generate a normal method call for it.

        out << generateMethodBody("        ",   // indent
                                  className, smokeClassName, meth, index, dynamicDispatch, includes);
    } else {
        // This is a virtual method. To know whether we should call with dynamic dispatch, we need a bit of RTTI magic.
        includes.insert("typeinfo");
        out << "        if (dynamic_cast<__internal_SmokeClass*>(static_cast<" << className << "*>(this))) {\n";   //
        out << generateMethodBody("            ",   // indent
                                  className, smokeClassName, meth, index, false, includes);
        out << "        } else {\n";
        out << generateMethodBody("            ",   // indent
                                  className, smokeClassName, meth, index, true, includes);
        out << "        }\n";
    }

    out << "    }\n";
    
    // If the constructor was generated from another one with default parameteres, we don't need to explicitly create
    // it here again. The x_* call will append the default parameters at the end and thus choose the right constructor.
    if (meth.isConstructor() && meth.remainingDefaultValues().isEmpty()) {
        out << "    explicit " << smokeClassName << '(';
        QStringList x_list;
        for (int i = 0; i < meth.parameters().count(); i++) {
            if (i > 0) out << ", ";
            Type *paramType = meth.parameters()[i].type();
            QString paramName = " x" + QString::number(i + 1);
            out << paramType->toString(paramName);
            if (!paramType->isFunctionPointer())
              out << paramName;
            x_list << "x" + QString::number(i + 1);
        }
        out << ") : " << meth.getClass()->name() << '(' << x_list.join(", ") << ") {}\n";
    }
}

void SmokeClassFiles::generateGetAccessor(QTextStream& out, const QString& className, const Field& field,
                                          const Type* type, int index)
{
    out << "    ";
    QString fieldName;
    if (field.flags() & Field::Static) {
        out << "static ";
    } else {
        fieldName = "this->";
    }
    fieldName += className + "::" + field.name();
    out << "void x_" << index << "(Smoke::Stack x) {\n"
        << "        // " << field.toString() << "\n"
        << "        x[0]." << Util::stackItemField(type) << " = "
            << Util::assignmentString(type, fieldName) << ";\n"
        << "    }\n";
}

void SmokeClassFiles::generateSetAccessor(QTextStream& out, const QString& className, const Field& field,
                                          const Type* type, int index)
{
    out << "    ";
    QString fieldName;
    if (field.flags() & Field::Static) {
        out << "static ";
    } else {
        fieldName = "this->";
    }
    fieldName += className + "::" + field.name();
    out << "void x_" << index << "(Smoke::Stack x) {\n"
        << "        // " << field.toString() << "=\n"
        << "        " << fieldName << " = ";
    QString unionField = Util::stackItemField(type);
    QString cast = type->toString();
    cast.replace("&", "");
    if (unionField == "s_class" && type->pointerDepth() == 0) {
        out << '*';
        cast += '*';
    }
    out << '(' << cast << ')' << "x[1]." << unionField << ";\n";
    out << "    }\n";
}

void SmokeClassFiles::generateEnumMemberCall(QTextStream& out, const QString& className, const QString& member, int index)
{
    out << "    static void x_" << index << "(Smoke::Stack x) {\n"
        << "        x[0].s_enum = (long)";
    
    if (!className.isEmpty())
        out  << className << "::";
    
    out << member << ";\n"
        << "    }\n";
}

void SmokeClassFiles::generateVirtualMethod(QTextStream& out, const Method& meth, QSet<QString>& includes)
{
    QString x_params, x_list;
    QString type = meth.type()->toString();
    if (meth.type()->getClass())
        includes.insert(meth.type()->getClass()->fileName());
    
    out << "    virtual " << type << " " << meth.name() << "(";
    for (int i = 0; i < meth.parameters().count(); i++) {
        if (i > 0) { out << ", "; x_list.append(", "); }
        const Parameter& param = meth.parameters()[i];
        
        if (param.type()->getClass())
            includes.insert(param.type()->getClass()->fileName());
        
        out << param.type()->toString() << " x" << i + 1;
        x_params += QString("        x[%1].%2 = %3;\n")
            .arg(QString::number(i + 1)).arg(Util::stackItemField(param.type()))
            .arg(Util::assignmentString(param.type(), "x" + QString::number(i + 1)));
        x_list += "x" + QString::number(i + 1);
    }
    out << ") ";
    if (meth.isConst())
        out << "const ";
    if (meth.hasExceptionSpec()) {
        out << "throw(";
        for (int i = 0; i < meth.exceptionTypes().count(); i++) {
            if (i > 0) out << ", ";
            out << meth.exceptionTypes()[i].toString();
        }
        out << ") ";
    }
    out << "{\n";
    out << QString("        Smoke::StackItem x[%1];\n").arg(meth.parameters().count() + 1);
    out << x_params;
    
    if (meth.flags() & Method::PureVirtual) {
        out << QString("        this->_binding->callMethod(%1, (void*)this, x, true /*pure virtual*/);\n").arg(m_smokeData->methodIdx[&meth]);
        if (meth.type() != Type::Void) {
            QString field = Util::stackItemField(meth.type());
            if (meth.type()->pointerDepth() == 0 && field == "s_class") {
                QString tmpType = type;
                if (meth.type()->isRef()) tmpType.replace('&', "");
                tmpType.append('*');
                out << "        " << tmpType << " xptr = (" << tmpType << ")x[0].s_class;\n";
                out << "        " << type << " xret(*xptr);\n";
                out << "        delete xptr;\n";
                out << "        return xret;\n";
            } else {
                out << QString("        return (%1)x[0].%2;\n").arg(type, Util::stackItemField(meth.type()));
            }
        }
    } else {
        out << QString("        if (this->_binding->callMethod(%1, (void*)this, x)) ").arg(m_smokeData->methodIdx[&meth]);
        if (meth.type() == Type::Void) {
            out << "return;\n";
        } else {
            QString field = Util::stackItemField(meth.type());
            if (meth.type()->pointerDepth() == 0 && field == "s_class") {
                QString tmpType = type;
                if (meth.type()->isRef()) tmpType.replace('&', "");
                tmpType.append('*');
                out << "{\n";
                out << "            " << tmpType << " xptr = (" << tmpType << ")x[0].s_class;\n";
                out << "            " << type << " xret(*xptr);\n";
                out << "            delete xptr;\n";
                out << "            return xret;\n";
                out << "        }\n";
            } else {
                out << QString("return (%1)x[0].%2;\n").arg(type, Util::stackItemField(meth.type()));
            }
        }
        out << "        ";
        if (meth.type() != Type::Void)
            out << "return ";
        out << QString("this->%1::%2(%3);\n").arg(meth.getClass()->toString()).arg(meth.name()).arg(x_list);
    }
    out << "    }\n";
}

void SmokeClassFiles::writeClass(QTextStream& out, const Class* klass, const QString& className, QSet<QString>& includes)
{
    const QString underscoreName = QString(className).replace("::", "__");
    const QString smokeClassName = "x_" + underscoreName;

    QString switchCode;
    QTextStream switchOut(&switchCode);

    if (klass->kind() == Class::Kind_Union) {
      // xcall_class function
      out << "void xcall_" << underscoreName << "(Smoke::Index xi, void *obj, Smoke::Stack args) {\n";
      out << "    " << className << " *xself = (" << className << "*)obj;\n";
      out << "    switch(xi) {\n";
      out << switchCode;
      if (Util::hasClassPublicDestructor(klass))
        out << "        case " << 1 << ": delete (" << className << "*)xself;\tbreak;\n";
      out << "    }\n";
      out << "}\n";
      return;
    }
    
    out << QString("class %1").arg(smokeClassName);
    if (!klass->isNameSpace()) {
        out << QString(" : public %1").arg(className);
        if (Util::hasClassVirtualDestructor(klass) && Util::hasClassPublicDestructor(klass)) {
            out << ", public __internal_SmokeClass";
        }
    }
    out << " {\n";
    if (Util::canClassBeInstanciated(klass)) {
        out << "    SmokeBinding* _binding;\n";
        out << "public:\n";
        out << "    void x_0(Smoke::Stack x) {\n";
        out << "        // set the smoke binding\n";
        out << "        _binding = (SmokeBinding*)x[1].s_class;\n";
        out << "    }\n";
        
        switchOut << "        case 0: xself->x_0(args);\tbreak;\n";
    } else {
        out << "public:\n";
    }
    
    int xcall_index = 1;
    const Method *destructor = 0;
    foreach (const Method& meth, klass->methods()) {
        if (meth.access() == Access_private)
            continue;
        if (meth.isDestructor()) {
            destructor = &meth;
            continue;
        }
        switchOut << "        case " << xcall_index << ": "
                  << (((meth.flags() & Method::Static) || meth.isConstructor()) ? smokeClassName + "::" : "xself->")
                  << "x_" << xcall_index << "(args);\tbreak;\n";
        if (Util::fieldAccessors.contains(&meth)) {
            // accessor method?
            const Field* field = Util::fieldAccessors[&meth];
            if (meth.name().startsWith("set")) {
                generateSetAccessor(out, className, *field, meth.parameters()[0].type(), xcall_index);
            } else {
                generateGetAccessor(out, className, *field, meth.type(), xcall_index);
            }
        } else {
            generateMethod(out, className, smokeClassName, meth, xcall_index, includes);
        }
        xcall_index++;
    }

    QString enumCode;
    QTextStream enumOut(&enumCode);
    const Enum* e = 0;
    bool enumFound = false;
    foreach (const BasicTypeDeclaration* decl, klass->children()) {
        if (!(e = dynamic_cast<const Enum*>(decl)))
            continue;
        if (e->access() == Access_private)
            continue;
        
        foreach (const EnumMember& member, e->members()) {
            switchOut << "        case " << xcall_index << ": " << smokeClassName <<  "::x_" << xcall_index << "(args);\tbreak;\n";
            if (e->parent())
                generateEnumMemberCall(out, className, member.name(), xcall_index++);
            else
                generateEnumMemberCall(out, e->nameSpace(), member.name(), xcall_index++);
        }
        
        // only generate the xenum_call if the enum has a valid name
        if (e->name().isEmpty())
            continue;
        
        enumFound = true;
        
        // xenum_operation method code
        QString enumString = e->toString();
        enumOut << "        case " << m_smokeData->typeIndex[&types[enumString]] << ": //" << enumString << '\n';
        enumOut << "            switch(xop) {\n";
        enumOut << "                case Smoke::EnumNew:\n";
        enumOut << "                    xdata = (void*)new " << enumString << ";\n";
        enumOut << "                    break;\n";
        enumOut << "                case Smoke::EnumDelete:\n";
        enumOut << "                    delete (" << enumString << "*)xdata;\n";
        enumOut << "                    break;\n";
        enumOut << "                case Smoke::EnumFromLong:\n";
        enumOut << "                    *(" << enumString << "*)xdata = (" << enumString << ")xvalue;\n";
        enumOut << "                    break;\n";
        enumOut << "                case Smoke::EnumToLong:\n";
        enumOut << "                    xvalue = (long)*(" << enumString << "*)xdata;\n";
        enumOut << "                    break;\n";
        enumOut << "            }\n";
        enumOut << "            break;\n";
    }
    
    foreach (const Method* meth, Util::virtualMethodsForClass(klass)) {
        generateVirtualMethod(out, *meth, includes);
    }
    
    // this class contains enums, write out an xenum_operation method
    if (enumFound) {
        out << "    static void xenum_operation(Smoke::EnumOperation xop, Smoke::Index xtype, void *&xdata, long &xvalue) {\n";
        out << "        switch(xtype) {\n";
        out << enumCode;
        out << "        }\n";
        out << "    }\n";
    }
    
    // destructor
    
    // if the class can't be instanciated, a callback when it's
    // deleted is unnecessary, but we still generate a private
    // destructor declaration, because otherwise gcc (under recent
    // standards) will unsuccessfully attempt to generate a destructor
    if (Util::canClassBeInstanciated(klass)) {
        out << "    ~" << smokeClassName << "() ";
        if (destructor && destructor->hasExceptionSpec()) {
            out << "throw(";
            for (int i = 0; i < destructor->exceptionTypes().count(); i++) {
                if (i > 0) out << ", ";
                out << destructor->exceptionTypes()[i].toString();
            }
            out << ") ";
        }
        out << QString("{ this->_binding->deleted(%1, (void*)this); }\n").arg(m_smokeData->classIndex[className]);
    } else {
        out << "private:\n    ~" << smokeClassName << "();\n";
    }
    out << "};\n";
    
    if (enumFound) {
        out << "void xenum_" << underscoreName << "(Smoke::EnumOperation xop, Smoke::Index xtype, void *&xdata, long &xvalue) {\n";
        out << "    " << smokeClassName << "::xenum_operation(xop, xtype, xdata, xvalue);\n";
        out << "}\n";
    }
    
    // xcall_class function
    out << "void xcall_" << underscoreName << "(Smoke::Index xi, void *obj, Smoke::Stack args) {\n";
    out << "    " << smokeClassName << " *xself = (" << smokeClassName << "*)obj;\n";
    out << "    switch(xi) {\n";
    out << switchCode;
    if (Util::hasClassPublicDestructor(klass))
        out << "        case " << xcall_index << ": delete (" << className << "*)xself;\tbreak;\n";
    out << "    }\n";
    out << "}\n";
}
