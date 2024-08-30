#include <QNetworkInterface>
#include "mainwindow.h"
#include "ui_mainwindow.h"

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent), ui(new Ui::MainWindow)
{
    ui->setupUi(this);
    ui->statusBar->showMessage("Server is not listening");
}

MainWindow::~MainWindow()
{
    foreach (QTcpSocket* socket, connection_set)
    {
        socket->close();
        socket->deleteLater();
    }

    m_server->close();
    m_server->deleteLater();

    delete ui;
}

void MainWindow::newConnection()
{
    while (m_server->hasPendingConnections())
        appendToSocketList(m_server->nextPendingConnection());
}

void MainWindow::appendToSocketList(QTcpSocket* socket)
{
    connection_set.insert(socket);

    connect(socket, &QTcpSocket::readyRead, this, &MainWindow::readSocket);
    connect(socket, &QTcpSocket::disconnected, this, &MainWindow::discardSocket);
    connect(socket, &QAbstractSocket::errorOccurred, this, &MainWindow::displayError);

    displayMessage(QString("INFO :: Client with sockd:%1 has just entered the room").arg(socket->socketDescriptor()));
    sendMessageAll(QString("INFO :: Client with sockd:%1 has just entered the room").arg(socket->socketDescriptor()));

    ui->receiverBox->addItem(QString::number(socket->socketDescriptor()));
}

void MainWindow::on_setPort_clicked(){
    m_server = new QTcpServer();

    quint16 port = ui->port->text().toInt();
    QString ip;

    const QHostAddress &localhost = QHostAddress(QHostAddress::LocalHost);
    for (const QHostAddress &address: QNetworkInterface::allAddresses()) {
        if (address.protocol() == QAbstractSocket::IPv4Protocol && address != localhost)
            ip = address.toString();
    }

    if(m_server->listen(QHostAddress::Any, port) && port != 0){
       connect(this, &MainWindow::newMessage, this, &MainWindow::displayMessage);
       connect(m_server, &QTcpServer::newConnection, this, &MainWindow::newConnection);
       ui->statusBar->showMessage("Server is listening");
       displayMessage(ip);
       displayMessage(QString::number(port));
    }
    else
        QMessageBox::critical(this,"QTCPServer",QString("Unable to start the server: %1.").arg(m_server->errorString()));
}

void MainWindow::readSocket()
{
    QTcpSocket* socket = reinterpret_cast<QTcpSocket*>(sender());

    QByteArray buffer;

    QDataStream socketStream(socket);
    socketStream.setVersion(QDataStream::Qt_5_15);

    socketStream.startTransaction();
    socketStream >> buffer;

    if(!socketStream.commitTransaction()){
        QString message = QString("%1 :: Waiting for more data to come..").arg(socket->socketDescriptor());
        emit newMessage(message);
        return;
    }

    QString header = buffer.mid(0,128);
    QString fileType = header.split(",")[0].split(":")[1];

    buffer = buffer.mid(128);

    if(fileType=="attachment"){
        QString fileName = header.split(",")[1].split(":")[1];
        QString ext = fileName.split(".")[1];
        QString size = header.split(",")[2].split(":")[1].split(";")[0];

        QString filePath = QFileDialog::getSaveFileName(this, tr("Save File"), QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)+"/"+fileName, QString("File (*.%1)").arg(ext));

        QFile file(filePath);
        if(file.open(QIODevice::WriteOnly)){
            file.write(buffer);
            QString message = QString("INFO :: Attachment from sd:%1 successfully stored on disk under the path %2").arg(socket->socketDescriptor()).arg(QString(filePath));
            emit newMessage(message);
        }else
            QMessageBox::critical(this,"QTCPServer", "An error occurred while trying to write the attachment.");

        auto it = connection_set.begin();
        foreach (QTcpSocket* socket1,connection_set){
            if(socket->socketDescriptor() != socket1->socketDescriptor()){
                sendAttachment(*it, filePath);
            }
            ++it;
        }
        QString message = QString("INFO :: Attachment from sd:%1 discarded").arg(socket->socketDescriptor());
        emit newMessage(message);
    }else if(fileType=="message"){
        QString message = QString("%1 :: %2").arg(socket->socketDescriptor()).arg(QString::fromStdString(buffer.toStdString()));
        sendMessageAll(message);
        emit newMessage(message);
    }
}

void MainWindow::discardSocket()
{
    QTcpSocket* socket = reinterpret_cast<QTcpSocket*>(sender());
    QSet<QTcpSocket*>::iterator it = connection_set.find(socket);
    if (it != connection_set.end()){
        displayMessage(QString("INFO :: A client has just left the room").arg(socket->socketDescriptor()));
        connection_set.remove(*it);
    }
    refreshComboBox();

    socket->close();
    socket->deleteLater();
}

void MainWindow::displayError(QAbstractSocket::SocketError socketError)
{
    switch (socketError) {
        case QAbstractSocket::RemoteHostClosedError:
            break;
        case QAbstractSocket::HostNotFoundError:
            QMessageBox::information(this, "QTCPServer", "The host was not found. Please check the host name and port settings.");
            break;
        case QAbstractSocket::ConnectionRefusedError:
            QMessageBox::information(this, "QTCPServer", "The connection was refused by the peer. Make sure QTCPServer is running, and check that the host name and port settings are correct.");
            break;
        default:
            QTcpSocket* socket = qobject_cast<QTcpSocket*>(sender());
            QMessageBox::information(this, "QTCPServer", QString("The following error occurred: %1.").arg(socket->errorString()));
            break;
    }
}

void MainWindow::sendMessage(QTcpSocket* socket, QString message){
    if(socket){
        if(socket->isOpen()){
            QDataStream socketStream(socket);
            socketStream.setVersion(QDataStream::Qt_5_15);

            QByteArray header;
            header.prepend(QString("fileType:message,fileName:null,fileSize:%1;").arg(message.size()).toUtf8());
            header.resize(128);

            QByteArray byteArray = message.toUtf8();
            byteArray.prepend(header);

            socketStream.setVersion(QDataStream::Qt_5_15);
            socketStream << byteArray;
        }else
            QMessageBox::critical(this,"QTCPServer","Socket doesn't seem to be opened");
    }else
        QMessageBox::critical(this,"QTCPServer","Not connected");
}

void MainWindow::sendMessageAll(QString message){
    auto it = connection_set.begin();
    for(int i = 0; i < connection_set.size(); ++i){
        sendMessage(*it, message);
        ++it;
    }
}

void MainWindow::sendAttachment(QTcpSocket* socket, QString filePath)
{
    if(socket){
        if(socket->isOpen()){
            QFile m_file(filePath);
            if(m_file.open(QIODevice::ReadOnly)){

                QFileInfo fileInfo(m_file.fileName());
                QString fileName(fileInfo.fileName());

                QDataStream socketStream(socket);
                socketStream.setVersion(QDataStream::Qt_5_15);

                QByteArray header;
                header.prepend(QString("fileType:attachment,fileName:%1,fileSize:%2;").arg(fileName).arg(m_file.size()).toUtf8());
                header.resize(128);

                QByteArray byteArray = m_file.readAll();
                byteArray.prepend(header);

                socketStream << byteArray;
            }else
                QMessageBox::critical(this,"QTCPClient","Couldn't open the attachment!");
        }else
            QMessageBox::critical(this,"QTCPServer","Socket doesn't seem to be opened");
    }else
        QMessageBox::critical(this,"QTCPServer","Not connected");
}


void MainWindow::displayMessage(const QString& str)
{
    ui->textBrowser_receivedMessages->append(str);
}

void MainWindow::refreshComboBox(){
    ui->receiverBox->clear();
    ui->receiverBox->addItem("None");
    foreach(QTcpSocket* socket, connection_set)
        ui->receiverBox->addItem(QString::number(socket->socketDescriptor()));
}

void MainWindow::on_KickButton_clicked()
{
    foreach (QTcpSocket* socket,connection_set){
        if(socket->socketDescriptor() == ui->receiverBox->currentText().toLongLong()){
            socket->close();
            socket->deleteLater();
            connection_set.remove(socket);

            refreshComboBox();

            break;
        }
    }
}

