#include "packager.h"
#include "operations/operation.h"

#include <qtlog.h>
#include <QCryptographicHash>
#include <QProcess>
#include <QTemporaryFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDebug>
#include <QRunnable>
#include <QThreadPool>
#include <QTemporaryDir>
#include <QScopedPointer>
#include <QElapsedTimer>
#include <QJsonArray>

Packager::Packager(QObject *parent) : QObject(parent)
{

}

/**
   \brief Generate a new patch from old source to new source
   The generation is made of 5 sequentials steps :
   \list 1
    \li Check packager configuration
    \li Compare directories
    \li Construct operations (use a thread pool to speed up creation time)
    \li Construct the final package
    \li Save package metadata
   \endlist
 */
void Packager::generate()
{
    try
    {
        QElapsedTimer globalTimer, stepTimer;
        globalTimer.start();
        stepTimer = globalTimer;

        LOG_TRACE(tr("Checking packager configuration..."));

        if(newDirectoryPath().isEmpty())
            throw tr("New directory path is empty");

        if(newRevisionName().isEmpty())
            throw tr("New revision name is empty");

        QDir oldDir(oldDirectoryPath());
        if(!oldDir.exists() && !oldDirectoryPath().isEmpty())
            throw tr("Old directory doesn't exists");

        QDir newDir(newDirectoryPath());
        if(!newDir.exists())
            throw tr("New directory doesn't exists");

        QFile deltaFile(deltaFilename());
        if(deltaFile.exists())
            throw tr("Delta file already exists");
        if(!deltaFile.open(QFile::WriteOnly))
            throw tr("Unable to create new delta file");

        QFile metadataFile(deltaMetaDataFilename());
        if(metadataFile.exists())
            throw tr("Delta metadata file already exists");
        if(!metadataFile.open(QFile::WriteOnly | QFile::Text))
            throw tr("Unable to create new delta metadata file");

        LOG_TRACE(tr("Packager configuration checked in %1").arg(stepTimer.restart()));

        LOG_TRACE(tr("Comparing directories %1 against %2").arg(newDirectoryPath(), oldDirectoryPath()));
        {
            QFileInfoList newFiles = dirList(newDir);
            QFileInfoList oldFiles = oldDirectoryPath().isNull() ? QFileInfoList() : dirList(oldDir);
            m_tasks.clear();
            compareDirectories(QStringLiteral(""), newFiles, oldFiles);
        }
        LOG_TRACE(tr("Directory comparison done in %1").arg(stepTimer.restart()));

        LOG_TRACE(tr("Creating operations..."));
        {
            QThreadPool threadPool;
            for(size_t i = 0; i < m_tasks.size(); ++i)
            {
                PackagerTask & task = m_tasks[i];
                task.tmpDirectory = tmpDirectoryPath();
                if(task.isRunSlow())
                    threadPool.start(&task);
                else
                    task.run();
            }
            threadPool.waitForDone();
        }
        LOG_INFO(tr("Operations created in %1").arg(stepTimer.restart()));

        LOG_TRACE(tr("Creating final delta file..."));
        PackageMetadata metadata;
        {
            qint64 totalSize = 0;
            qint64 read;
            char buffer[8096];
            QFile operationFile;
            for(size_t i = 0; i < m_tasks.size(); ++i)
            {
                PackagerTask & task = m_tasks[i];
                if(!task.errorString.isNull())
                    throw task.errorString;

                if(task.operation->size() > 0)
                {
                    operationFile.setFileName(task.operation->dataFilename());
                    if(!operationFile.open(QFile::ReadOnly))
                        throw tr("Unable to open %1").arg(operationFile.fileName());

                    while((read = operationFile.read(buffer, sizeof(buffer))) > 0)
                    {
                        if(deltaFile.write(buffer, read) != read)
                             throw tr("Unable to write %1").arg(deltaFile.fileName());
                    }

                    operationFile.close();
                    task.operation->setOffset(totalSize);
                    totalSize += task.operation->size();
                }
                metadata.addOperation(task.operation);
            }
            metadata.setPackage(Package(newRevisionName(), oldRevisionName(), totalSize));

            if(!deltaFile.flush())
                 throw tr("Unable to flush %1").arg(deltaFile.fileName());

            deltaFile.close();
        }
        LOG_INFO(tr("Final delta file created in %1").arg(stepTimer.restart()));

        LOG_INFO(tr("Writing metadata"));
        metadataFile.write(QJsonDocument(metadata.toJsonObject()).toJson(QJsonDocument::Indented));
        LOG_INFO(tr("Metadata written in %1").arg(stepTimer.restart()));

        LOG_INFO(tr("Delta creation succeded in %1").arg(globalTimer.elapsed()));
    }
    catch (const QString & reason)
    {
        LOG_INFO(tr("Delta creation failed %1").arg(reason));
    }
}

QFileInfoList Packager::dirList(const QDir & dir)
{
    QFileInfoList list = dir.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden, QDir::Name);
    for (int i = list.size()-1; i >= 0; --i)
    {
        if (list.at(i).fileName() == QLatin1String(".git"))
            list.removeAt(i);
    }
    return list;
}


void Packager::addRemoveDirTask(QString path, QFileInfo &pathInfo)
{
    QFileInfoList files = dirList(QDir(pathInfo.absoluteFilePath()));
    for(int pos = 0, len = files.size(); pos < len; ++pos)
    {
        QFileInfo & file = files[pos];
        if(file.isDir())
        {
            addRemoveDirTask(path+QLatin1Char('/')+file.fileName(), file);
        }
        else
        {
            addTask(PackagerTask::RemoveFile, path+QLatin1Char('/')+file.fileName());
        }
    }
    addTask(PackagerTask::RemoveDir, path);
}

void Packager::compareDirectories(QString path, const QFileInfoList & newFiles, const QFileInfoList & oldFiles)
{
    int newPos = 0, newLen = newFiles.size();
    int oldPos = 0, oldLen = oldFiles.size();

    LOG_TRACE(tr("path = %1, newFiles.size() = %2, oldFiles.size() = %3").arg(path).arg(newLen).arg(oldLen));

    while(newPos < newLen || oldPos < oldLen)
    {
        QFileInfo newFile, oldFile;
        int diff;
        if(newPos < newLen)
            newFile = newFiles[newPos];
        if(oldPos < oldLen)
            oldFile = oldFiles[oldPos];

        if(newPos < newLen && oldPos < oldLen)
            diff = QString::compare(newFile.fileName(), oldFile.fileName());
        else
            diff = newPos < newLen ? -1 : 1;

        LOG_TRACE(tr("diff = %1, newFile = %2, oldFile = %3").arg(diff).arg(newFile.filePath()).arg(oldFile.filePath()));

        if(diff < 0)
        {
            if(newFile.isFile())
            {
                // Add newFile
                addTask(PackagerTask::Add, path+newFile.fileName(), newFile.filePath());
            }
            else if(newFile.isDir())
            {
                compareDirectories(path + newFile.fileName() + QLatin1Char('/'),
                                   dirList(QDir(newFile.filePath())), QFileInfoList());
            }
            ++newPos;
        }
        else if(diff > 0)
        {
            // Del oldFile
            if(oldFile.isDir())
                addRemoveDirTask(path + oldFile.fileName(), oldFile);
            else
                addTask(PackagerTask::RemoveFile, path + newFile.fileName());
            ++oldPos;
        }
        else // diff == 0
        {
            if(newFile.isFile())
            {
                if(!oldFile.isFile())
                {
                    // RMD + ADD
                    addRemoveDirTask(path + oldFile.fileName(), oldFile);
                    //m_tasks.emplace_back(PackagerTask(PackagerTask::RemoveDir, path + newFile.fileName(), QString(), newFile.filePath()));
                    addTask(PackagerTask::Add, path + newFile.fileName(), newFile.filePath());
                }
                else
                {
                    // Make diff
                    addTask(PackagerTask::Patch, path + newFile.fileName(), newFile.filePath(), oldFile.filePath());
                }
            }
            else if(newFile.isDir())
            {
                if(!oldFile.isDir())
                {
                    addTask(PackagerTask::RemoveFile, path + newFile.fileName());
                    compareDirectories(path + newFile.fileName() + QLatin1Char('/'),
                                       dirList(QDir(newFile.filePath())),
                                       QFileInfoList());
                }
                else
                {
                    compareDirectories(path + newFile.fileName() + QLatin1Char('/'),
                                       dirList(QDir(newFile.filePath())),
                                       dirList(QDir(oldFile.filePath())));
                }
            }

            ++newPos;
            ++oldPos;
        }

    }

    LOG_TRACE(tr("generate_recursion done"));
}

void Packager::addTask(PackagerTask::Type operationType, QString path, QString newFilename, QString oldFilename)
{
    m_tasks.push_back(PackagerTask(operationType, path, oldFilename, newFilename));
}

