/*  This file is part of Akonadi EWS Resource
    Copyright (C) 2015-2016 Krzysztof Nowicki <krissn@op.pl>

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Library General Public
    License as published by the Free Software Foundation; either
    version 2 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Library General Public License for more details.

    You should have received a copy of the GNU Library General Public License
    along with this library; see the file COPYING.LIB.  If not, write to
    the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
    Boston, MA 02110-1301, USA.
*/

#ifndef EWSITEM_H
#define EWSITEM_H

#include <QtCore/QXmlStreamReader>
#include <QtCore/QList>

#include "ewsitembase.h"

class EwsItemPrivate;

class EwsItem : public EwsItemBase
{
public:
    typedef QList<EwsItem> List;
    typedef QMultiMap<QString, QString> HeaderMap;

    EwsItem(QXmlStreamReader &reader);
    EwsItem(const EwsItem &other);
    EwsItem(EwsItem &&other);
    virtual ~EwsItem();

    EwsItem& operator=(const EwsItem &other);
    EwsItem& operator=(EwsItem &&other);

    EwsItemType type() const;
protected:
    bool readBaseItemElement(QXmlStreamReader &reader);
    bool readMessageHeaders(QXmlStreamReader &reader);
    bool readRecipients(QXmlStreamReader &reader, EwsItemFields field);
    bool readBoolean(QXmlStreamReader &reader, EwsItemFields field);
};

Q_DECLARE_METATYPE(EwsItem::HeaderMap)

#endif
