#include "package.h"
#include "jsonutil.h"

#include <QObject>

Package::Package()
{
    size = -1;
}

Package::Package(const QString &to, const QString &from, qint64 size)
{
    this->to = to;
    this->from = from;
    this->size = size;
}

QString Package::url() const
{
    if(from.isEmpty())
        return QStringLiteral("complete_%1").arg(to);
    return QStringLiteral("patch%1_%2").arg(from, to);
}

void Package::fromJsonObjectV1(const QJsonObject &packageObject)
{
    from = JsonUtil::asString(packageObject, QStringLiteral("from"));
    to = JsonUtil::asString(packageObject, QStringLiteral("to"));
    bool ok;
    size = JsonUtil::asString(packageObject, QStringLiteral("size")).toLongLong(&ok);
    if(!ok)
        throw(QObject::tr("package 'size' is not a qint64 string"));
}

QJsonObject Package::toJsonObjectV1() const
{
    QJsonObject packageObject;
    packageObject.insert(QStringLiteral("from"), from);
    packageObject.insert(QStringLiteral("to"), to);
    packageObject.insert(QStringLiteral("size"), QString::number(size));

    return packageObject;
}
