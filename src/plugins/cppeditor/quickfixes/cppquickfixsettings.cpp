// Copyright (C) 2020 Leander Schulten <Leander.Schulten@rwth-aachen.de>
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#include "cppquickfixsettings.h"

#include "cppquickfixprojectsettings.h"

#include "../cppeditorconstants.h"

#include <coreplugin/icore.h>
#include <coreplugin/jsexpander.h>

#include <projectexplorer/projecttree.h>

#include <utils/qtcsettings.h>

#include <QRegularExpression>

using namespace Utils;

namespace CppEditor {

CppQuickFixSettings::CppQuickFixSettings(bool loadGlobalSettings)
{
    setDefaultSettings();
    if (loadGlobalSettings)
        this->loadGlobalSettings();
}

void CppQuickFixSettings::loadGlobalSettings()
{
    loadSettingsFrom(Core::ICore::settings());
}

void CppQuickFixSettings::loadSettingsFrom(QtcSettings *s)
{
    CppQuickFixSettings def;
    s->beginGroup(Constants::QUICK_FIX_SETTINGS_ID);
    getterOutsideClassFrom = s->value(Constants::QUICK_FIX_SETTING_GETTER_OUTSIDE_CLASS_FROM,
                                      def.getterOutsideClassFrom)
                                 .toInt();
    getterInCppFileFrom = s->value(Constants::QUICK_FIX_SETTING_GETTER_IN_CPP_FILE_FROM,
                                   def.getterInCppFileFrom)
                              .toInt();
    setterOutsideClassFrom = s->value(Constants::QUICK_FIX_SETTING_SETTER_OUTSIDE_CLASS_FROM,
                                      def.setterOutsideClassFrom)
                                 .toInt();
    setterInCppFileFrom = s->value(Constants::QUICK_FIX_SETTING_SETTER_IN_CPP_FILE_FROM,
                                   def.setterInCppFileFrom)
                              .toInt();
    getterAttributes
        = s->value(Constants::QUICK_FIX_SETTING_GETTER_ATTRIBUTES, def.getterAttributes).toString();
    getterNameTemplate = s->value(Constants::QUICK_FIX_SETTING_GETTER_NAME_TEMPLATE,
                                  def.getterNameTemplate)
                             .toString();
    setterNameTemplate = s->value(Constants::QUICK_FIX_SETTING_SETTER_NAME_TEMPLATE,
                                  def.setterNameTemplate)
                             .toString();
    setterParameterNameTemplate = s->value(Constants::QUICK_FIX_SETTING_SETTER_PARAMETER_NAME,
                                           def.setterParameterNameTemplate)
                                      .toString();
    resetNameTemplate = s->value(Constants::QUICK_FIX_SETTING_RESET_NAME_TEMPLATE,
                                 def.resetNameTemplate)
                            .toString();
    signalNameTemplate = s->value(Constants::QUICK_FIX_SETTING_SIGNAL_NAME_TEMPLATE,
                                  def.signalNameTemplate)
                             .toString();
    signalWithNewValue = s->value(Constants::QUICK_FIX_SETTING_SIGNAL_WITH_NEW_VALUE,
                                  def.signalWithNewValue)
                             .toBool();
    setterAsSlot = s->value(Constants::QUICK_FIX_SETTING_SETTER_AS_SLOT, def.setterAsSlot).toBool();
    cppFileNamespaceHandling = static_cast<MissingNamespaceHandling>(
        s->value(Constants::QUICK_FIX_SETTING_CPP_FILE_NAMESPACE_HANDLING,
                 static_cast<int>(def.cppFileNamespaceHandling))
            .toInt());
    useAuto = s->value(Constants::QUICK_FIX_SETTING_USE_AUTO, def.useAuto).toBool();

    memberVariableNameTemplate = s->value(Constants::QUICK_FIX_SETTING_MEMBER_VARIABLE_NAME_TEMPLATE,
                                          def.memberVariableNameTemplate)
                                     .toString();
    nameFromMemberVariableTemplate
        = s->value(Constants::QUICK_FIX_SETTING_REVERSE_MEMBER_VARIABLE_NAME_TEMPLATE,
                   def.nameFromMemberVariableTemplate)
              .toString();
    valueTypes = s->value(Constants::QUICK_FIX_SETTING_VALUE_TYPES, def.valueTypes).toStringList();
    returnByConstRef = s->value(Constants::QUICK_FIX_SETTING_RETURN_BY_CONST_REF,
                                def.returnByConstRef).toBool();
    customTemplates = def.customTemplates;
    int size = s->beginReadArray(Constants::QUICK_FIX_SETTING_CUSTOM_TEMPLATES);
    if (size > 0)
        customTemplates.clear();

    for (int i = 0; i < size; ++i) {
        s->setArrayIndex(i);
        CustomTemplate c;
        c.types = s->value(Constants::QUICK_FIX_SETTING_CUSTOM_TEMPLATE_TYPES).toStringList();
        if (c.types.isEmpty())
            continue;
        c.equalComparison = s->value(Constants::QUICK_FIX_SETTING_CUSTOM_TEMPLATE_COMPARISON)
                                .toString();
        c.returnType = s->value(Constants::QUICK_FIX_SETTING_CUSTOM_TEMPLATE_RETURN_TYPE).toString();
        c.returnExpression
            = s->value(Constants::QUICK_FIX_SETTING_CUSTOM_TEMPLATE_RETURN_EXPRESSION).toString();
        c.assignment = s->value(Constants::QUICK_FIX_SETTING_CUSTOM_TEMPLATE_ASSIGNMENT).toString();
        if (c.assignment.isEmpty() && c.returnType.isEmpty() && c.equalComparison.isEmpty())
            continue; // nothing custom here

        customTemplates.push_back(c);
    }
    s->endArray();
    s->endGroup();
}

void CppQuickFixSettings::saveSettingsTo(QtcSettings *s)
{
    CppQuickFixSettings def;
    s->beginGroup(Constants::QUICK_FIX_SETTINGS_ID);
    s->setValueWithDefault(Constants::QUICK_FIX_SETTING_GETTER_OUTSIDE_CLASS_FROM,
                           getterOutsideClassFrom,
                           def.getterOutsideClassFrom);
    s->setValueWithDefault(Constants::QUICK_FIX_SETTING_GETTER_IN_CPP_FILE_FROM,
                           getterInCppFileFrom,
                           def.getterInCppFileFrom);
    s->setValueWithDefault(Constants::QUICK_FIX_SETTING_SETTER_OUTSIDE_CLASS_FROM,
                           setterOutsideClassFrom,
                           def.setterOutsideClassFrom);
    s->setValueWithDefault(Constants::QUICK_FIX_SETTING_SETTER_IN_CPP_FILE_FROM,
                           setterInCppFileFrom,
                           def.setterInCppFileFrom);

    s->setValueWithDefault(Constants::QUICK_FIX_SETTING_GETTER_ATTRIBUTES,
                           getterAttributes,
                           def.getterAttributes);
    s->setValueWithDefault(Constants::QUICK_FIX_SETTING_GETTER_NAME_TEMPLATE,
                           getterNameTemplate,
                           def.getterNameTemplate);
    s->setValueWithDefault(Constants::QUICK_FIX_SETTING_SETTER_NAME_TEMPLATE,
                           setterNameTemplate,
                           def.setterNameTemplate);
    s->setValueWithDefault(Constants::QUICK_FIX_SETTING_RESET_NAME_TEMPLATE,
                           resetNameTemplate,
                           def.resetNameTemplate);
    s->setValueWithDefault(Constants::QUICK_FIX_SETTING_SIGNAL_NAME_TEMPLATE,
                           signalNameTemplate,
                           def.signalNameTemplate);
    s->setValueWithDefault(Constants::QUICK_FIX_SETTING_SIGNAL_WITH_NEW_VALUE,
                           signalWithNewValue,
                           def.signalWithNewValue);
    s->setValueWithDefault(Constants::QUICK_FIX_SETTING_CPP_FILE_NAMESPACE_HANDLING,
                           int(cppFileNamespaceHandling),
                           int(def.cppFileNamespaceHandling));
    s->setValueWithDefault(Constants::QUICK_FIX_SETTING_MEMBER_VARIABLE_NAME_TEMPLATE,
                           memberVariableNameTemplate,
                           def.memberVariableNameTemplate);
    s->setValueWithDefault(Constants::QUICK_FIX_SETTING_REVERSE_MEMBER_VARIABLE_NAME_TEMPLATE,
                           nameFromMemberVariableTemplate,
                           def.nameFromMemberVariableTemplate);
    s->setValueWithDefault(Constants::QUICK_FIX_SETTING_SETTER_PARAMETER_NAME,
                           setterParameterNameTemplate,
                           def.setterParameterNameTemplate);
    s->setValueWithDefault(Constants::QUICK_FIX_SETTING_SETTER_AS_SLOT,
                           setterAsSlot,
                           def.setterAsSlot);
    s->setValueWithDefault(Constants::QUICK_FIX_SETTING_USE_AUTO,
                           useAuto,
                           def.useAuto);
    s->setValueWithDefault(Constants::QUICK_FIX_SETTING_VALUE_TYPES,
                           valueTypes,
                           def.valueTypes);
    s->setValueWithDefault(Constants::QUICK_FIX_SETTING_RETURN_BY_CONST_REF,
                           returnByConstRef,
                           def.returnByConstRef);
    if (customTemplates == def.customTemplates) {
        s->remove(Constants::QUICK_FIX_SETTING_CUSTOM_TEMPLATES);
    } else {
        s->beginWriteArray(Constants::QUICK_FIX_SETTING_CUSTOM_TEMPLATES);
        for (int i = 0; i < static_cast<int>(customTemplates.size()); ++i) {
            const auto &c = customTemplates[i];
            s->setArrayIndex(i);
            s->setValue(Constants::QUICK_FIX_SETTING_CUSTOM_TEMPLATE_TYPES, c.types);
            s->setValue(Constants::QUICK_FIX_SETTING_CUSTOM_TEMPLATE_COMPARISON, c.equalComparison);
            s->setValue(Constants::QUICK_FIX_SETTING_CUSTOM_TEMPLATE_RETURN_TYPE, c.returnType);
            s->setValue(Constants::QUICK_FIX_SETTING_CUSTOM_TEMPLATE_RETURN_EXPRESSION,
                        c.returnExpression);
            s->setValue(Constants::QUICK_FIX_SETTING_CUSTOM_TEMPLATE_ASSIGNMENT, c.assignment);
        }
        s->endArray();
    }
    s->endGroup();
}

void CppQuickFixSettings::saveAsGlobalSettings()
{
    saveSettingsTo(Core::ICore::settings());
}

void CppQuickFixSettings::setDefaultSettings()
{
    valueTypes << "Pointer"     // for Q...Pointer
               << "optional"    // for ...::optional
               << "unique_ptr"; // for std::unique_ptr and boost::movelib::unique_ptr
    valueTypes << "int"
               << "long"
               << "char"
               << "real"
               << "short"
               << "unsigned"
               << "size"
               << "float"
               << "double"
               << "bool";
    CustomTemplate floatingPoint;
    floatingPoint.types << "float"
                        << "double"
                        << "qreal"
                        << "long double";
    floatingPoint.equalComparison = "qFuzzyCompare(<cur>, <new>)";
    customTemplates.push_back(floatingPoint);

    CustomTemplate unique_ptr;
    unique_ptr.types << "unique_ptr";
    unique_ptr.assignment = "<cur> = std::move(<new>)";
    unique_ptr.returnType = "<T>*";
    unique_ptr.returnExpression = "<cur>.get()";
    customTemplates.push_back(unique_ptr);
}

QString CppQuickFixSettings::memberBaseName(
    const QString &name, const std::optional<QString> &baseNameTemplate)
{
    const auto validName = [](const QString &name) {
        return !name.isEmpty() && !name.at(0).isDigit();
    };
    QString baseName = name;

    QString baseNameTemplateValue;
    if (baseNameTemplate) {
        baseNameTemplateValue = *baseNameTemplate;
    } else {
        const CppQuickFixSettings *const settings
            = Internal::CppQuickFixProjectsSettings::getQuickFixSettings(
                ProjectExplorer::ProjectTree::currentProject());
        baseNameTemplateValue = settings->nameFromMemberVariableTemplate;
    }
    if (!baseNameTemplateValue.isEmpty())
        return CppQuickFixSettings::replaceNamePlaceholders(baseNameTemplateValue, name, {});

    // Remove leading and trailing "_"
    while (baseName.startsWith(QLatin1Char('_')))
        baseName.remove(0, 1);
    while (baseName.endsWith(QLatin1Char('_')))
        baseName.chop(1);
    if (baseName != name && validName(baseName))
        return baseName;

    // If no leading/trailing "_": remove "m_" and "m" prefix
    if (baseName.startsWith(QLatin1String("m_"))) {
        baseName.remove(0, 2);
    } else if (baseName.startsWith(QLatin1Char('m')) && baseName.length() > 1
               && baseName.at(1).isUpper()) {
        baseName.remove(0, 1);
        baseName[0] = baseName.at(0).toLower();
    }

    return validName(baseName) ? baseName : name;

}

QString CppQuickFixSettings::replaceNamePlaceholders(
    const QString &nameTemplate, const QString &name, const std::optional<QString> &memberName)
{
    Core::JsExpander expander;
    QString jsError;
    QString jsExpr;
    if (memberName) {
        QTC_CHECK(!memberName->isEmpty());
        jsExpr = QString("(function(name, memberName) { return %1; })(\"%2\", \"%3\")")
                     .arg(nameTemplate, name, *memberName);
    } else {
        jsExpr = QString("(function(name) { return %1; })(\"%2\")").arg(nameTemplate, name);
    }
    const QString jsRes = expander.evaluate(jsExpr, &jsError);
    if (!jsError.isEmpty())
        return jsError; // TODO: Use Utils::Result?
    return jsRes;
}

auto removeAndExtractTemplate(QString type)
{
    // maybe we have somethink like: myName::test<std::byte>::fancy<std::optional<int>>, then we want fancy
    QString realType;
    QString templateParameter;
    int counter = 0;
    int start = 0;
    int templateStart = 0;
    for (int i = 0; i < type.length(); ++i) {
        auto c = type[i];
        if (c == '<') {
            if (counter == 0) {
                // template start
                realType += type.mid(start, i - start);
                templateStart = i + 1;
            }
            ++counter;
        } else if (c == '>') {
            --counter;
            if (counter == 0) {
                // template ends
                start = i + 1;
                templateParameter = type.mid(templateStart, i - templateStart);
            }
        }
    }
    if (start < type.length()) // add last block if there is one
        realType += type.mid(start);

    struct _
    {
        QString type;
        QString templateParameter;
    };
    return _{realType, templateParameter};
}

auto withoutNamespace(QString type)
{
    const auto namespaceIndex = type.lastIndexOf("::");
    if (namespaceIndex >= 0)
        return type.mid(namespaceIndex + 2);
    return type;
}

bool CppQuickFixSettings::isValueType(QString type) const
{
    // first remove template stuff
    auto realType = removeAndExtractTemplate(type).type;
    // remove namespaces: namespace_int::complex should not be matched by int
    realType = withoutNamespace(realType);
    for (const auto &valueType : valueTypes) {
        if (realType.contains(valueType))
            return true;
    }
    return false;
}

void CppQuickFixSettings::GetterSetterTemplate::replacePlaceholders(QString currentValueVariableName,
                                                                    QString newValueVariableName)
{
    equalComparison = equalComparison.replace("<new>", newValueVariableName)
                          .replace("<cur>", currentValueVariableName);
    assignment = assignment.replace("<new>", newValueVariableName)
                     .replace("<cur>", currentValueVariableName);
    returnExpression = returnExpression.replace("<new>", newValueVariableName)
                           .replace("<cur>", currentValueVariableName);
}

CppQuickFixSettings::GetterSetterTemplate CppQuickFixSettings::findGetterSetterTemplate(
    QString fullyQualifiedType) const
{
    const int index = fullyQualifiedType.lastIndexOf("::");
    const QString namespaces = index >= 0 ? fullyQualifiedType.left(index) : "";
    const QString typeOnly = index >= 0 ? fullyQualifiedType.mid(index + 2) : fullyQualifiedType;
    CustomTemplate bestMatch;
    enum MatchType { FullyExact, FullyContains, Exact, Contains, None } currentMatch = None;
    QRegularExpression regex;
    for (const auto &cTemplate : customTemplates) {
        for (const auto &t : cTemplate.types) {
            QString type = t;
            bool fully = false;
            if (t.contains("::")) {
                const int index = t.lastIndexOf("::");
                if (t.left(index) != namespaces)
                    continue;

                type = t.mid(index + 2);
                fully = true;
            } else if (currentMatch <= FullyContains) {
                continue;
            }

            MatchType match = None;
            if (t.contains("*")) {
                regex.setPattern('^' + QString(t).replace('*', ".*") + '$');
                if (regex.match(typeOnly).isValid() != QRegularExpression::NormalMatch)
                    match = fully ? FullyContains : Contains;
            } else if (t == typeOnly) {
                match = fully ? FullyExact : Exact;
            }
            if (match < currentMatch) {
                currentMatch = match;
                bestMatch = cTemplate;
            }
        }
    }

    if (currentMatch != None) {
        GetterSetterTemplate t;
        if (!bestMatch.equalComparison.isEmpty())
            t.equalComparison = bestMatch.equalComparison;
        if (!bestMatch.returnExpression.isEmpty())
            t.returnExpression = bestMatch.returnExpression;
        if (!bestMatch.assignment.isEmpty())
            t.assignment = bestMatch.assignment;
        if (!bestMatch.returnType.isEmpty())
            t.returnTypeTemplate = bestMatch.returnType;
        return t;
    }
    return GetterSetterTemplate{};
}

CppQuickFixSettings::FunctionLocation CppQuickFixSettings::determineGetterLocation(int lineCount) const
{
    int outsideDiff = getterOutsideClassFrom > 0 ? lineCount - getterOutsideClassFrom : -1;
    int cppDiff = getterInCppFileFrom > 0 ? lineCount - getterInCppFileFrom : -1;
    if (outsideDiff > cppDiff) {
        if (outsideDiff >= 0)
            return FunctionLocation::OutsideClass;
    }
    return cppDiff >= 0 ? FunctionLocation::CppFile : FunctionLocation::InsideClass;
}

CppQuickFixSettings::FunctionLocation CppQuickFixSettings::determineSetterLocation(int lineCount) const
{
    int outsideDiff = setterOutsideClassFrom > 0 ? lineCount - setterOutsideClassFrom : -1;
    int cppDiff = setterInCppFileFrom > 0 ? lineCount - setterInCppFileFrom : -1;
    if (outsideDiff > cppDiff) {
        if (outsideDiff >= 0)
            return FunctionLocation::OutsideClass;
    }
    return cppDiff >= 0 ? FunctionLocation::CppFile : FunctionLocation::InsideClass;
}

} // namespace CppEditor
