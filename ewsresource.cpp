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

#include <QtCore/QDebug>

#include <KI18n/KLocalizedString>
#include <AkonadiCore/ChangeRecorder>
#include <AkonadiCore/CollectionFetchScope>
#include <AkonadiCore/ItemFetchScope>
#include <AkonadiCore/CollectionFetchJob>
#include <KMime/Message>

#include "ewsresource.h"
#include "ewsfetchitemsjob.h"
#include "ewsfetchfoldersjob.h"
#include "ewsgetitemrequest.h"
#include "ewsupdateitemrequest.h"
#include "ewsmoveitemrequest.h"
#include "ewssubscriptionmanager.h"
#include "ewsgetfolderrequest.h"
#include "ewsitemhandler.h"
#include "ewsmodifyitemjob.h"
#include "configdialog.h"
#include "settings.h"
#include "ewsclient_debug.h"

#include "resourceadaptor.h"

using namespace Akonadi;

EwsResource::EwsResource(const QString &id)
    : Akonadi::ResourceBase(id)
{
    qDebug() << "EwsResource";
    //setName(i18n("Microsoft Exchange"));
    mEwsClient.setUrl(Settings::baseUrl());
    mEwsClient.setCredentials(Settings::username(), Settings::password());

    changeRecorder()->fetchCollection(true);
    changeRecorder()->collectionFetchScope().setAncestorRetrieval(CollectionFetchScope::Parent);
    changeRecorder()->itemFetchScope().fetchFullPayload(true);
    changeRecorder()->itemFetchScope().setAncestorRetrieval(ItemFetchScope::Parent);
    changeRecorder()->itemFetchScope().setFetchModificationTime(false);

    mRootCollection.setParentCollection(Collection::root());
    mRootCollection.setName(name());
    mRootCollection.setContentMimeTypes(QStringList() << Collection::mimeType() << KMime::Message::mimeType());
    mRootCollection.setRights(Collection::ReadOnly);

    resetUrl();

    // Load the sync state
    QByteArray data = QByteArray::fromBase64(Settings::self()->syncState().toAscii());
    if (!data.isEmpty()) {
        data = qUncompress(data);
        if (!data.isEmpty()) {
            QDataStream stream(data);
            stream >> mSyncState;
            qDebug() << mSyncState;
        }
    }

    Q_EMIT status(0);

    QMetaObject::invokeMethod(this, "delayedInit", Qt::QueuedConnection);
}

EwsResource::~EwsResource()
{
}

void EwsResource::delayedInit()
{
    new ResourceAdaptor(this);
}

void EwsResource::resetUrl()
{
    EwsGetFolderRequest *req = new EwsGetFolderRequest(mEwsClient, this);
    req->setFolderId(EwsId(EwsDIdMsgFolderRoot));
    EwsFolderShape shape(EwsShapeIdOnly);
    shape << EwsPropertyField(QStringLiteral("folder:DisplayName"));
    req->setFolderShape(shape);
    connect(req, &EwsRequest::result, this, &EwsResource::rootFolderFetchFinished);
    req->start();
}

void EwsResource::rootFolderFetchFinished(KJob *job)
{
    qDebug() << "rootFolderFetchFinished";
    EwsGetFolderRequest *req = qobject_cast<EwsGetFolderRequest*>(job);
    if (!req) {
        Q_EMIT status(AgentBase::Broken, i18n("Unable to connect to Exchange server"));
        qCWarning(EWSRES_LOG) << QStringLiteral("Invalid job object");
        return;
    }

    if (req->error()) {
        Q_EMIT status(AgentBase::Broken, i18n("Unable to connect to Exchange server"));
        qWarning() << "ERROR" << req->errorString();
        return;
    }

    EwsId id = req->folder()[EwsFolderFieldFolderId].value<EwsId>();
    if (id.type() == EwsId::Real) {
        mRootCollection.setRemoteId(id.id());
        mRootCollection.setRemoteRevision(id.changeKey());
        qDebug() << "Root folder is " << id;
        Q_EMIT status(AgentBase::Running);

        mSubManager.reset(new EwsSubscriptionManager(mEwsClient, id, this));
        connect(mSubManager.data(), &EwsSubscriptionManager::foldersModified, this, &EwsResource::foldersModifiedEvent);
        connect(mSubManager.data(), &EwsSubscriptionManager::folderTreeModified, this, &EwsResource::folderTreeModifiedEvent);
        connect(mSubManager.data(), &EwsSubscriptionManager::fullSyncRequested, this, &EwsResource::fullSyncRequestedEvent);
        mSubManager->start();
    }
}

void EwsResource::retrieveCollections()
{
    qDebug() << "retrieveCollections";
    if (mRootCollection.remoteId().isNull()) {
        cancelTask(QStringLiteral("Root folder id not known."));
        return;
    }

    EwsFetchFoldersJob *job = new EwsFetchFoldersJob(mEwsClient, mFolderSyncState,
        mRootCollection, this);
    connect(job, &EwsFetchFoldersJob::result, this, &EwsResource::findFoldersRequestFinished);
    job->start();
}

void EwsResource::retrieveItems(const Collection &collection)
{
    Q_EMIT status(1, QStringLiteral("Retrieving item list"));
    qDebug() << "retrieveItems";

    EwsFetchItemsJob *job = new EwsFetchItemsJob(collection, mEwsClient,
        mSyncState.value(collection.remoteId()), this);
    connect(job, SIGNAL(finished(KJob*)), SLOT(itemFetchJobFinished(KJob*)));
    connect(job, SIGNAL(status(int,const QString&)), SIGNAL(status(int,const QString&)));
    connect(job, SIGNAL(percent(int)), SIGNAL(percent(int)));
    job->start();
}

bool EwsResource::retrieveItem(const Akonadi::Item &item, const QSet<QByteArray> &parts)
{
    Q_UNUSED(parts)

    qDebug() << "retrieveItem";
    EwsGetItemRequest *req = new EwsGetItemRequest(mEwsClient, this);
    EwsId::List ids;
    ids << EwsId(item.remoteId(), item.remoteRevision());
    req->setItemIds(ids);
    EwsItemShape shape(EwsShapeIdOnly);
    shape << EwsPropertyField("item:MimeContent");
    req->setItemShape(shape);
    req->setProperty("item", QVariant::fromValue<Item>(item));
    connect(req, &EwsGetItemRequest::result, req,
            [this](KJob *job){getItemRequestFinished(qobject_cast<EwsGetItemRequest*>(job));});
    req->start();
    return true;
}

void EwsResource::configure(WId windowId)
{
    ConfigDialog dlg(this, windowId);
    if (dlg.exec()) {
        mSubManager.reset(Q_NULLPTR);
        mEwsClient.setUrl(Settings::baseUrl());
        mEwsClient.setCredentials(Settings::username(), Settings::password());
        Settings::self()->save();
        resetUrl();
    }
}

void EwsResource::findFoldersRequestFinished(KJob *job)
{
    qDebug() << "findFoldersRequestFinished";
    EwsFetchFoldersJob *req = qobject_cast<EwsFetchFoldersJob*>(job);
    if (!req) {
        qCWarning(EWSRES_LOG) << QStringLiteral("Invalid job object");
        cancelTask(QStringLiteral("Invalid job object"));
        return;
    }

    if (req->error()) {
        qWarning() << "ERROR" << req->errorString();
        cancelTask(req->errorString());
        return;
    }

    mFolderSyncState = req->syncState();
    if (req->fullSync()) {
        collectionsRetrieved(req->folders());
        qCDebug(EWSRES_LOG) << req->folders();
    }
    else {
        collectionsRetrievedIncremental(req->changedFolders(), req->deletedFolders());
        qCDebug(EWSRES_LOG) << req->changedFolders() << req->deletedFolders();
    }
}

void EwsResource::itemFetchJobFinished(KJob *job)
{
    EwsFetchItemsJob *fetchJob = qobject_cast<EwsFetchItemsJob*>(job);

    if (!fetchJob) {
        qCWarningNC(EWSRES_LOG) << QStringLiteral("Invalid job object");
        cancelTask(QStringLiteral("Invalid job object"));
        return;
    }
    if (job->error()) {
        qCWarningNC(EWSRES_LOG) << QStringLiteral("Item fetch error:") << job->errorString();
        if (mSyncState.contains(fetchJob->collection().remoteId())) {
            qCDebugNC(EWSRES_LOG) << QStringLiteral("Retrying with empty state.");
            // Retry with a clear sync state.
            mSyncState.remove(fetchJob->collection().remoteId());
            retrieveItems(fetchJob->collection());
        }
        else {
            qCDebugNC(EWSRES_LOG) << QStringLiteral("Clean sync failed.");
            // No more hope
            cancelTask(job->errorString());
            return;
        }
    }
    else {
        mSyncState[fetchJob->collection().remoteId()] = fetchJob->syncState();
        itemsRetrievedIncremental(fetchJob->changedItems(), fetchJob->deletedItems());
    }
    saveState();
    Q_EMIT status(0);
}

void EwsResource::getItemRequestFinished(EwsGetItemRequest *req)
{
    qDebug() << "getItemRequestFinished";

    if (req->error()) {
        qWarning() << "ERROR" << req->errorString();
        cancelTask(req->errorString());
        return;
    }

    Item item = req->property("item").value<Item>();
    const EwsGetItemRequest::Response &resp = req->responses()[0];
    if (!resp.isSuccess()) {
        qWarning() << QStringLiteral("Item fetch failed!");
        cancelTask(QStringLiteral("Item fetch failed!"));
        return;
    }
    const EwsItem &ewsItem = resp.item();
    if (!EwsItemHandler::itemHandler(ewsItem.internalType())->setItemPayload(item, ewsItem)) {
        cancelTask(QStringLiteral("Failed to fetch item payload."));
        return;
    }

    itemRetrieved(item);
}

void EwsResource::itemChanged(const Akonadi::Item &item, const QSet<QByteArray> &partIdentifiers)
{
    EwsItemType type = EwsItemHandler::mimeToItemType(item.mimeType());
    if (type == EwsItemTypeItem) {
        cancelTask("Item type not supported for changing");
    }
    else {
        EwsModifyItemJob *job = EwsItemHandler::itemHandler(type)->modifyItemJob(mEwsClient, item,
            partIdentifiers, this);
        connect(job, SIGNAL(result(KJob*)), SLOT(itemChangeRequestFinished(KJob*)));
        job->start();
    }
}

void EwsResource::itemChangeRequestFinished(KJob *job)
{
    if (job->error()) {
        cancelTask(job->errorString());
        return;
    }

    EwsModifyItemJob *req = qobject_cast<EwsModifyItemJob*>(job);
    if (!req) {
        cancelTask(QStringLiteral("Invalid job object"));
        return;
    }

    changeCommitted(req->item());
}

void EwsResource::itemsMoved(const Item::List &items, const Collection &sourceCollection,
                             const Collection &destinationCollection)
{
    Q_UNUSED(sourceCollection);

    EwsId::List ids;
    Q_FOREACH(const Item &item, items) {
        EwsId id(item.remoteId(), item.remoteRevision());
        ids.append(id);
    }

    EwsMoveItemRequest *req = new EwsMoveItemRequest(mEwsClient, this);
    req->setItemIds(ids);
    EwsId destId(destinationCollection.remoteId(), QString());
    req->setDestinationFolderId(destId);
    req->setProperty("items", QVariant::fromValue<Item::List>(items));
    connect(req, SIGNAL(result(KJob*)), SLOT(itemMoveRequestFinished(KJob*)));
    req->start();
}

void EwsResource::itemMoveRequestFinished(KJob *job)
{
    if (job->error()) {
        cancelTask(job->errorString());
        return;
    }

    EwsMoveItemRequest *req = qobject_cast<EwsMoveItemRequest*>(job);
    if (!req) {
        cancelTask(QStringLiteral("Invalid job object"));
        return;
    }
    Item::List items = job->property("items").value<Item::List>();

    if (items.count() != req->responses().count()) {
        cancelTask(QStringLiteral("Invalid number of responses received from server."));
        return;
    }

    Item::List::iterator it = items.begin();
    Q_FOREACH(const EwsMoveItemRequest::Response &resp, req->responses()) {
        Item &item = *it;
        if (resp.isSuccess()) {
            if (item.isValid()) {
                item.setRemoteRevision(resp.itemId().changeKey());
            }

        }
        qDebug() << "changeCommitted" << item.remoteId();
        changeCommitted(item);
        it++;
    }
}

void EwsResource::foldersModifiedEvent(EwsId::List folders)
{
    Q_FOREACH(const EwsId &id, folders) {
        Collection c;
        c.setRemoteId(id.id());
        CollectionFetchJob *job = new CollectionFetchJob(c, CollectionFetchJob::Base);
        job->setFetchScope(changeRecorder()->collectionFetchScope());
        job->fetchScope().setResource(identifier());
        job->fetchScope().setListFilter(CollectionFetchScope::Sync);
        connect(job, SIGNAL(result(KJob*)), SLOT(foldersModifiedCollectionSyncFinished(KJob*)));
    }

}

void EwsResource::foldersModifiedCollectionSyncFinished(KJob *job)
{
    qCDebugNC(EWSRES_LOG) << job->error() << job->errorText();
    if (job->error()) {
        qCDebug(EWSRES_LOG) << QStringLiteral("Failed to fetch collection tree for sync.");
        return;
    }

    CollectionFetchJob *fetchJob = qobject_cast<CollectionFetchJob*>(job);
    synchronizeCollection(fetchJob->collections()[0].id());
}

void EwsResource::folderTreeModifiedEvent()
{
    synchronizeCollectionTree();
}

void EwsResource::fullSyncRequestedEvent()
{
    synchronize();
}

void EwsResource::clearSyncState()
{
    mSyncState.clear();
}

void EwsResource::saveState()
{
    QByteArray str;
    QDataStream dataStream(&str, QIODevice::WriteOnly);
    dataStream << mSyncState;
    Settings::self()->setSyncState(qCompress(str, 9).toBase64());
    Settings::self()->save();
}

AKONADI_RESOURCE_MAIN(EwsResource)
