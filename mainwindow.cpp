/**********************************************************************
 *  MainWindow.cpp
 **********************************************************************
 * Copyright (C) 2017 MX Authors
 *
 * Authors: Adrian
 *          Dolphin_Oracle
 *          MX Linux <http://mxlinux.org>
 *
 * This file is part of mx-packageinstaller.
 *
 * mx-packageinstaller is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * mx-packageinstaller is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with mx-packageinstaller.  If not, see <http://www.gnu.org/licenses/>.
 **********************************************************************/

#include <QFileDialog>
#include <QImageReader>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QProgressBar>
#include <QSettings>
#include <QScrollBar>
#include <QTextStream>
#include <QtXml/QtXml>

#include <QDebug>

#include "mainwindow.h"
#include "ui_mainwindow.h"

#include "about.h"
#include "aptcache.h"
#include "versionnumber.h"
#include "version.h"

MainWindow::MainWindow(QWidget *parent) :
    QDialog(parent),
    ui(new Ui::MainWindow),
    dictionary("/usr/share/mx-packageinstaller-pkglist/category.dict", QSettings::IniFormat)
{
    qDebug().noquote() << QCoreApplication::applicationName() << "version:" << VERSION;

    ui->setupUi(this);
    setProgressDialog();

    connect(&timer, &QTimer::timeout, this, &MainWindow::updateBar, Qt::UniqueConnection);
    connect(&cmd, &Cmd::started, this, &MainWindow::cmdStart, Qt::UniqueConnection);
    connect(&cmd, &Cmd::finished, this, &MainWindow::cmdDone, Qt::UniqueConnection);
    conn = connect(&cmd, &Cmd::outputAvailable, [](const QString &out) { qDebug() << out.trimmed(); });
    connect(&cmd, &Cmd::errorAvailable, [](const QString &out) { qWarning() << out.trimmed(); });
    setWindowFlags(Qt::Window); // for the close, min and max buttons
    setup();
}

MainWindow::~MainWindow()
{
    delete ui;
}

// Setup versious items first time program runs
void MainWindow::setup()
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";
    ui->tabWidget->blockSignals(true);

    QFont font("monospace");
    font.setStyleHint(QFont::Monospace);
    ui->outputBox->setFont(font);

    fp_ver = getVersion("flatpak");
    user = "--system ";

    if (system("arch | grep -q x86_64") == 0) {
        arch = "amd64";
    } else {
        arch = "i386";
    }
    QString ver_num = getDebianVersion();
    if (ver_num == "8") {
        ver_name = "jessie";
    } else if (ver_num == "9") {
        ver_name = "stretch";
    } else if (ver_num == "10") {
        ver_name = "buster";
    }

    lock_file = new LockFile("/var/lib/dpkg/lock");
    connect(qApp, &QApplication::aboutToQuit, this, &MainWindow::cleanup, Qt::QueuedConnection);
    test_initially_enabled = cmd.run("grep -q '^deb.* test' /etc/apt/sources.list.d/mx.list");
    this->setWindowTitle(tr("MX Package Installer"));
    ui->tabWidget->setCurrentIndex(0);
    QStringList column_names;
    column_names << "" << "" << tr("Package") << tr("Info") << tr("Description");
    ui->treePopularApps->setHeaderLabels(column_names);
    ui->treeStable->hideColumn(5); // Status of the package: installed, upgradable, etc
    ui->treeStable->hideColumn(6); // Displayed status true/false
    ui->treeMXtest->hideColumn(5); // Status of the package: installed, upgradable, etc
    ui->treeMXtest->hideColumn(6); // Displayed status true/false
    ui->treeBackports->hideColumn(5); // Status of the package: installed, upgradable, etc
    ui->treeBackports->hideColumn(6); // Displayed status true/false
    ui->treeFlatpak->hideColumn(5); // Status
    ui->treeFlatpak->hideColumn(6); // Displayed
    ui->treeFlatpak->hideColumn(7); // Duplication
    ui->treeFlatpak->hideColumn(8); // Full string
    ui->icon->setIcon(QIcon::fromTheme("software-update-available", QIcon(":/icons/software-update-available.png")));
    ui->icon_2->setIcon(QIcon::fromTheme("software-update-available", QIcon(":/icons/software-update-available.png")));
    ui->icon_3->setIcon(QIcon::fromTheme("software-update-available", QIcon(":/icons/software-update-available.png")));
    loadPmFiles();
    refreshPopularApps();

    // connect search boxes
    connect(ui->searchPopular, &QLineEdit::textChanged, this, &MainWindow::findPopular);
    connect(ui->searchBoxStable, &QLineEdit::textChanged, this, &MainWindow::findPackageOther);
    connect(ui->searchBoxMX, &QLineEdit::textChanged, this, &MainWindow::findPackageOther);
    connect(ui->searchBoxBP, &QLineEdit::textChanged, this, &MainWindow::findPackageOther);
    connect(ui->searchBoxFlatpak, &QLineEdit::textChanged, this, &MainWindow::findPackageOther);

    // connect combo filters
    connect(ui->comboFilterStable, &QComboBox::currentTextChanged, this, &MainWindow::filterChanged);
    connect(ui->comboFilterMX, &QComboBox::currentTextChanged, this, &MainWindow::filterChanged);
    connect(ui->comboFilterBP, &QComboBox::currentTextChanged, this, &MainWindow::filterChanged);
    connect(ui->comboFilterFlatpak, &QComboBox::currentTextChanged, this, &MainWindow::filterChanged);

    ui->searchPopular->setFocus();
    updated_once = false;
    warning_backports = false;
    warning_flatpaks = false;
    warning_test = false;
    tree = ui->treePopularApps;
    ui->tabWidget->setTabEnabled(ui->tabWidget->indexOf(ui->tabOutput), false);
    ui->tabWidget->blockSignals(false);

    QSettings settings("mx-packageinstaller");
    restoreGeometry(settings.value("geometry").toByteArray());
}

// Uninstall listed packages
bool MainWindow::uninstall(const QString &names, const QString &postuninstall)
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";
    ui->tabWidget->setCurrentWidget(ui->tabOutput);

    lock_file->unlock();
    bool success;
    //simulate install of selections and present for confirmation
    //if user selects cancel, break routine but return success to avoid error message
    if (!confirmActions(names, "remove")) return true;

    ui->tabWidget->setTabText(ui->tabWidget->indexOf(ui->tabOutput), tr("Uninstalling packages..."));
    displayOutput();
    success = cmd.run("DEBIAN_FRONTEND=$(xprop -root | grep -sqi kde && echo kde || echo gnome) apt-get -o=Dpkg::Use-Pty=0 remove -y " + names); //use -y since there is a confirm dialog already
    if (!postuninstall.isEmpty()) {
        qDebug() << "Post-uninstall";
        ui->tabWidget->setTabText(ui->tabWidget->indexOf(ui->tabOutput), tr("Running post-uninstall operations..."));
        displayOutput();
        success = cmd.run(postuninstall);
    }
    lock_file->lock();

    return success;
}

// Run apt-get update
bool MainWindow::update()
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";
    QString msg;
    lock_file->unlock();
    if (!ui->tabOutput->isVisible()) { // don't display in output if calling to refresh from tabs
        progress->show();
    } else {
        ui->tabWidget->setTabText(ui->tabWidget->indexOf(ui->tabOutput), tr("Refreshing sources..."));
    }
    displayOutput();
    if (cmd.run("apt-get update -o=Dpkg::Use-Pty=0 -o Acquire::http:Timeout=10 -o Acquire::https:Timeout=10 -o Acquire::ftp:Timeout=10")) {
        lock_file->lock();
        msg="echo sources updated OK >>/var/log/mxpi.log";
        system(msg.toUtf8());
        updated_once = true;
        return true;
    }
    lock_file->lock();
    msg="echo problem updating sources >>/var/log/mxpi.log";
    system(msg.toUtf8());
    QMessageBox::critical(this, tr("Error"), tr("There was a problem updating sources. Some sources may not have provided updates. For more info check: ") +
                          "<a href=\"/var/log/mxpi.log\">/var/log/mxpi.log</a>");
    return false;
}


// convert number, unit to bytes
double MainWindow::convert(const double &number, const QString &unit) const
{
    if (unit == "KB") { // assuming KiB not KB
        return number * 1024;
    } else if (unit == "MB") {
        return number * 1024 * 1024;
    } else if (unit == "GB") {
        return number * 1024 * 1024 * 1024;
    } else { // for "bytes"
        return number;
    }
}


// Add sizes for the installed packages for older flatpak that doesn't list size for all the packages
void MainWindow::listSizeInstalledFP()
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";

    QString name, size;
    QTreeWidgetItemIterator it(ui->treeFlatpak);
    QString total = "0 bytes";
    QStringList list, runtimes;
    if (fp_ver < VersionNumber("1.0.1")) { // older version doesn't display all apps and runtimes without specifying them
        list = cmd.getCmdOut("runuser -l $(logname) -c \"flatpak -d list --app " + user + "|tr -s ' ' |cut -f1,5,6 -d' '\"").split("\n");
        runtimes = cmd.getCmdOut("runuser -l $(logname) -c \"flatpak -d list --runtime " + user + "|tr -s ' '|cut -f1,5,6 -d' '\"").split("\n");
        if (!runtimes.isEmpty()) {
            list << runtimes;
        }
        while (*it) {
            for (const QString &item : list) {
                name = item.section(" ", 0, 0);
                size = item.section(" ", 1);
                if (name == (*it)->text(8)) {
                    total = addSizes(total, size);
                    (*it)->setText(4, size);
                }
            }
            ++it;
        }
    } else if (fp_ver < VersionNumber("1.2.4")) {
        list = cmd.getCmdOut("runuser -l $(logname) -c \"flatpak -d list " + user + "|tr -s ' '|cut -f1,5\"").split("\n");
    } else {
        list = cmd.getCmdOut("runuser -l $(logname) -c \"flatpak list " + user + "--columns app,size\"").split("\n");
    }
    for (const QString &item : list) {
        total = addSizes(total, item.section("\t", 1));
    }

    ui->labelNumSize->setText(total);
}

// Block interface while updateing Flatpak list
void MainWindow::blockInterfaceFP(bool block)
{
    for (int tab = 0; tab < 4; ++tab) {
        ui->tabWidget->setTabEnabled(tab, !block);
    }
    ui->comboRemote->setDisabled(block);
    ui->comboFilterFlatpak->setDisabled(block);
    ui->comboUser->setDisabled(block);
    ui->searchBoxFlatpak->setDisabled(block);
    ui->treeFlatpak->setDisabled(block);
    ui->frameFP->setDisabled(block);
    ui->labelFP->setDisabled(block);
    ui->labelRepo->setDisabled(block);
    if (block) {
        setCursor(QCursor(Qt::BusyCursor));
    } else {
        setCursor(QCursor(Qt::ArrowCursor));
    }
}

// Update interface when done loading info
void MainWindow::updateInterface()
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";

    QList<QTreeWidgetItem *> upgr_list = tree->findItems("upgradable", Qt::MatchExactly, 5);
    QList<QTreeWidgetItem *> inst_list = tree->findItems("installed", Qt::MatchExactly, 5);

    if (tree == ui->treeStable) {
        ui->labelNumApps->setText(QString::number(tree->topLevelItemCount()));
        ui->labelNumUpgr->setText(QString::number(upgr_list.count()));
        ui->labelNumInst->setText(QString::number(inst_list.count() + upgr_list.count()));
        ui->buttonUpgradeAll->setVisible(upgr_list.count() > 0);
        ui->buttonForceUpdateStable->setEnabled(true);
        ui->searchBoxStable->setFocus();
    } else if (tree == ui->treeMXtest) {
        ui->labelNumApps_2->setText(QString::number(tree->topLevelItemCount()));
        ui->labelNumUpgrMX->setText(QString::number(upgr_list.count()));
        ui->labelNumInstMX->setText(QString::number(inst_list.count() + upgr_list.count()));
        ui->buttonForceUpdateMX->setEnabled(true);
        ui->searchBoxMX->setFocus();
    } else if (tree == ui->treeBackports) {
        ui->labelNumApps_3->setText(QString::number(tree->topLevelItemCount()));
        ui->labelNumUpgrBP->setText(QString::number(upgr_list.count()));
        ui->labelNumInstBP->setText(QString::number(inst_list.count() + upgr_list.count()));
        ui->buttonForceUpdateBP->setEnabled(true);
        ui->searchBoxBP->setFocus();
    }

    QApplication::setOverrideCursor(QCursor(Qt::ArrowCursor));
    progress->hide();
}


// add two string "00 KB" and "00 GB", return similar string
QString MainWindow::addSizes(QString arg1, QString arg2)
{
    QString number1 = arg1.section(" ", 0, 0);
    QString number2 = arg2.section(" ", 0, 0);
    QString unit1 = arg1.section(" ", 1);
    QString unit2 = arg2.section(" ", 1);

    double bytes = 0;
    // calculate
    bytes = convert(number1.toDouble(), unit1) + convert(number2.toDouble(), unit2);

    // presentation
    if (bytes < 1024) {
        return QString::number(bytes) + " bytes";
    } else if (bytes < 1024 * 1024) {
        return QString::number(bytes/1024) + " KB";
    } else if (bytes < 1024 * 1024 * 1024) {
        return QString::number(bytes/(1024 * 1024), 'f', 1) + " MB";
    } else {
        return QString::number(bytes/(1024 * 1024 * 1024), 'f', 2) + " GB";
    }
}

// Returns Debian main version number
QString MainWindow::getDebianVersion()
{
    return cmd.getCmdOut("cat /etc/debian_version | cut -f1 -d'.'");
}

// Returns localized name for elements
QString MainWindow::getLocalizedName(const QDomElement element) const
{
    // pass one, find fully localized string, e.g. "pt_BR"
    QDomElement child = element.firstChildElement();
    for (; (!child.isNull()); child = child.nextSiblingElement()) {
        if (child.tagName() == locale.name() && !child.text().trimmed().isEmpty()) {
            return child.text().trimmed();
        }
    }
    // pass two, find language, e.g. "pt"
    child = element.firstChildElement();
    for (; (!child.isNull()); child = child.nextSiblingElement()) {
        if (child.tagName() == locale.name().section("_", 0, 0) && !child.text().trimmed().isEmpty()) {
            return child.text().trimmed();
        }
    }
    // pass three, return "en" or "en_US"
    child = element.firstChildElement();
    for (; (!child.isNull()); child = child.nextSiblingElement()) {
        if ((child.tagName() == "en" || child.tagName() == "en_US") && !child.text().trimmed().isEmpty()) {
            return child.text().trimmed();
        }
    }

    child = element.firstChildElement();
    if (child.isNull()) {
        return element.text().trimmed(); // if no language tags are present
    } else {
        return child.text().trimmed(); // return first language tag if neither the specified locale nor "en" is found.
    }
}

// get translation for the category
QString MainWindow::getTranslation(const QString item)
{
    if (locale.name() == "en_US") { // no need for translation
        return item;
    }

    dictionary.beginGroup(item);

    QString trans = dictionary.value(locale.name()).toString().toLatin1(); // try pt_BR format
    if (trans.isEmpty()) {
        trans = dictionary.value(locale.name().section("_", 0, 0)).toString().toLatin1(); // try pt format
        if (trans.isEmpty()) {
            dictionary.endGroup();
            return item;  // return original item if no translation found
        }
    }
    dictionary.endGroup();
    return trans;
}

void MainWindow::updateBar()
{
    qApp->processEvents();
    if (bar->value() == 100) {
        bar->reset();
    }
    bar->setValue(bar->value() + 1);
}

void MainWindow::outputAvailable(const QString &output)
{
    if (output.contains("\r")) {
        ui->outputBox->moveCursor(QTextCursor::Up, QTextCursor::KeepAnchor);
        ui->outputBox->moveCursor(QTextCursor::EndOfLine, QTextCursor::KeepAnchor);
    }
    ui->outputBox->insertPlainText(output);
    ui->outputBox->verticalScrollBar()->setValue(ui->outputBox->verticalScrollBar()->maximum());
}


// Load info from the .pm files
void MainWindow::loadPmFiles()
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";
    QDomDocument doc;

    QStringList filter("*.pm");
    QDir dir("/usr/share/mx-packageinstaller-pkglist");
    const QStringList pmfilelist = dir.entryList(filter);

    for (const QString &file_name : pmfilelist) {
        QFile file(dir.absolutePath() + "/" + file_name);
        if (!file.open(QFile::ReadOnly | QFile::Text)) {
            qDebug() << "Could not open: " << file.fileName();
        } else {
            if (!doc.setContent(&file)) {
                qDebug() << "Could not load document: " << file_name << "-- not valid XML?";
            } else {
                processDoc(doc);
            }
        }
        file.close();
    }
}

// Process dom documents (from .pm files)
void MainWindow::processDoc(const QDomDocument &doc)
{
    /*  Items order in list:
            0 "category"
            1 "name"
            2 "description"
            3 "installable"
            4 "screenshot"
            5 "preinstall"
            6 "install_package_names"
            7 "postinstall"
            8 "uninstall_package_names"
            9 "postuninstall"
    */

    QString category;
    QString name;
    QString description;
    QString installable;
    QString screenshot;
    QString preinstall;
    QString postinstall;
    QString postuninstall;
    QString install_names;
    QString uninstall_names;
    QStringList list;

    QDomElement root = doc.firstChildElement("app");
    QDomElement element = root.firstChildElement();

    for (; !element.isNull(); element = element.nextSiblingElement()) {
        if (element.tagName() == "category") {
            category = getTranslation(element.text().trimmed());
        } else if (element.tagName() == "name") {
            name = element.text().trimmed();
        } else if (element.tagName() == "description") {
            description = getLocalizedName(element);
        } else if (element.tagName() == "installable") {
            installable = element.text().trimmed();
        } else if (element.tagName() == "screenshot") {
            screenshot = element.text().trimmed();
        } else if (element.tagName() == "preinstall") {
            preinstall = element.text().trimmed();
        } else if (element.tagName() == "install_package_names") {
            install_names = element.text().trimmed();
            install_names.replace("\n", " ");
        } else if (element.tagName() == "postinstall") {
            postinstall = element.text().trimmed();
        } else if (element.tagName() == "uninstall_package_names") {
            uninstall_names = element.text().trimmed();
        } else if (element.tagName() == "postuninstall") {
            postuninstall = element.text().trimmed();
        }
    }
    // skip non-installable packages
    if ((installable == "64" && arch != "amd64") || (installable == "32" && arch != "i386")) {
        return;
    }
    list << category << name << description << installable << screenshot << preinstall
         << postinstall << install_names << uninstall_names << postuninstall;
    popular_apps << list;
}

// Reload and refresh interface
void MainWindow::refreshPopularApps()
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";
    disableOutput();
    ui->treePopularApps->clear();
    ui->searchPopular->clear();
    ui->buttonInstall->setEnabled(false);
    ui->buttonUninstall->setEnabled(false);
    installed_packages = listInstalled();
    displayPopularApps();
}

// In case of duplicates add extra name to disambiguate
void MainWindow::removeDuplicatesFP()
{
    // find and mark duplicates
    QTreeWidgetItemIterator it(ui->treeFlatpak);
    QString current, next;
    while (*it) {
        current = ((*it))->text(1);\
        if (*(++it)) {
            next = ((*it))->text(1);
            if (next == current) {
                --it;
                (*(it))->setText(7, "Duplicate");
                ++it;
                (*it)->setText(7, "Duplicate");
            }
        }
    }
    // rename duplicate to use more context
    QTreeWidgetItemIterator it2(ui->treeFlatpak);
    while (*it2) {
        if ((*(it2))->text(7) == "Duplicate") {
            (*it2)->setText(1, (*it2)->text(2).section(".", -2));
        }
        ++it2;
    }
}

// Setup progress dialog
void MainWindow::setProgressDialog()
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";
    progress = new QProgressDialog(this);
    bar = new QProgressBar(progress);
    bar->setMaximum(100);
    progCancel = new QPushButton(tr("Cancel"));
    connect(progCancel, &QPushButton::clicked, this, &MainWindow::cancelDownload);
    progress->setWindowModality(Qt::WindowModal);
    progress->setWindowFlags(Qt::Dialog | Qt::CustomizeWindowHint | Qt::WindowTitleHint |Qt::WindowSystemMenuHint | Qt::WindowStaysOnTopHint);
    progress->setCancelButton(progCancel);
    progCancel->setDisabled(true);
    progress->setLabelText(tr("Please wait..."));
    progress->setAutoClose(false);
    progress->setBar(bar);
    bar->setTextVisible(false);
    progress->reset();
}

void MainWindow::setSearchFocus()
{
    if (ui->tabStable->isVisible()) {
        ui->searchBoxStable->setFocus();
    } else if (ui->tabMXtest->isVisible()) {
        ui->searchBoxMX->setFocus();
    } else if (ui->tabBackports->isVisible()) {
        ui->searchBoxBP->setFocus();
    } else if (ui->tabFlatpak->isVisible()) {
        ui->searchBoxFlatpak->setFocus();
    }
}

// Display Popular Apps in the treePopularApps
void MainWindow::displayPopularApps() const
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";
    QTreeWidgetItem *topLevelItem = nullptr;
    QTreeWidgetItem *childItem;

    for (const QStringList &list : popular_apps) {
        QString category = list.at(0);
        QString name = list.at(1);
        QString description = list.at(2);
        //QString installable = list.at(3);
        QString screenshot = list.at(4);
        //QString preinstall = list.at(5);
        //QString postinstall = list.at(6);
        QString install_names = list.at(7);
        QString uninstall_names = list.at(8);
        QString postuninstall = list.at(9);

        // add package category if treePopularApps doesn't already have it
        if (ui->treePopularApps->findItems(category, Qt::MatchFixedString, 2).isEmpty()) {
            topLevelItem = new QTreeWidgetItem();
            topLevelItem->setText(2, category);
            ui->treePopularApps->addTopLevelItem(topLevelItem);
            // topLevelItem look
            QFont font;
            font.setBold(true);
            topLevelItem->setFont(2, font);
            topLevelItem->setIcon(0, QIcon::fromTheme("folder"));
        } else {
            topLevelItem = ui->treePopularApps->findItems(category, Qt::MatchFixedString, 2).at(0); //find first match; add the child there
        }
        // add package name as childItem to treePopularApps
        childItem = new QTreeWidgetItem(topLevelItem);
        childItem->setText(2, name);
        childItem->setIcon(3, QIcon::fromTheme("dialog-information"));

        // add checkboxes
        childItem->setFlags(childItem->flags() | Qt::ItemIsUserCheckable);
        childItem->setCheckState(1, Qt::Unchecked);

        // add description from file
        childItem->setText(4, description);

        // add install_names (not displayed)
        childItem->setText(5, install_names);

        // add uninstall_names (not displayed)
        childItem->setText(6, uninstall_names);

        // add screenshot url (not displayed)
        childItem->setText(7, screenshot);

        // add postuinstall script (not displayed)
        childItem->setText(8, postuninstall);

        // gray out installed items
        if (checkInstalled(uninstall_names)) {
            childItem->setForeground(2, QBrush(Qt::gray));
            childItem->setForeground(4, QBrush(Qt::gray));
        }
    }
    for (int i = 0; i < 5; ++i) {
        ui->treePopularApps->resizeColumnToContents(i);
    }
    ui->treePopularApps->sortItems(2, Qt::AscendingOrder);
    connect(ui->treePopularApps, &QTreeWidget::itemClicked, this, &MainWindow::displayInfo, Qt::UniqueConnection);
}


// Display only the listed apps
void MainWindow::displayFiltered(const QStringList &list, bool raw) const
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";

    QStringList new_list;
    if (raw) { // raw format that needs to be edited
        if (fp_ver < VersionNumber("1.2.4")) {
            for (const QString &item : list) {
                new_list << item.section("\t", 0, 0); // remove size
            }
        } else {
            for (const QString &item : list) {
                new_list << item.section("\t", 1, 1).section("/", 1); // remove version and size
            }
        }
    } else {
        new_list = list;
    }
    int total = 0;
    QTreeWidgetItemIterator it(tree);
    while (*it) {
        if (new_list.contains((*it)->text(8))) {
            ++total;
            (*it)->setHidden(false);
            (*it)->setText(6, "true"); // Displayed flag
        } else {
            (*it)->setHidden(true);
            (*it)->setText(6, "false");
            (*it)->setCheckState(0, Qt::Unchecked); // uncheck hidden items
        }
        ++it;
    }
    ui->labelNumAppFP->setText(QString::number(total));
}


// Display available packages
void MainWindow::displayPackages()
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";
    bar->setValue(0);

    QTreeWidget *newtree; // use this to not overwrite current "tree"

    QMap<QString, QStringList> list;
    if (tree == ui->treeMXtest) {
        list = mx_list;
        newtree = ui->treeMXtest;
    } else if (tree == ui->treeBackports) {
        list = backports_list;
        newtree = ui->treeBackports;
    } else { // for ui-treeStable, ui->treePopularApps, ui->treeFlatpak
        list = stable_list;
        newtree = ui->treeStable;
    }

    newtree->blockSignals(true);

    QHash<QString, VersionNumber> hashInstalled = listInstalledVersions();
    QString app_name;
    QString app_info;
    QString app_ver;
    QString app_desc;
    VersionNumber installed;
    VersionNumber candidate;

    QTreeWidgetItem *widget_item;



    // create a list of apps, create a hash with app_name, app_info
    QMap<QString, QStringList>::iterator i;
    for (i = list.begin(); i != list.end(); ++i) {
        // get size for newer flatpak versions

        app_name = i.key();
        app_ver = i.value().at(0);
        app_desc = i.value().at(1);

        widget_item = new QTreeWidgetItem(tree);
        widget_item->setCheckState(0, Qt::Unchecked);
        widget_item->setText(2, app_name);
        widget_item->setText(3, app_ver);
        widget_item->setText(4, app_desc);
        widget_item->setText(6, "true"); // all items are displayed till filtered
    }
    for (int i = 0; i < newtree->columnCount(); ++i) {
        newtree->resizeColumnToContents(i);
    }

    // process the entire list of apps
    QTreeWidgetItemIterator it(newtree);
    int upgr_count = 0;
    int inst_count = 0;

    // update tree
    while (*it) {
        app_name = (*it)->text(2);
        if (isFilteredName(app_name) && ui->checkHideLibs->isChecked()) {
            (*it)->setHidden(true);
        }
        app_ver = (*it)->text(3);
        installed = hashInstalled.value(app_name);
        candidate = VersionNumber(list.value(app_name).at(0));
        VersionNumber repo_candidate(app_ver); // candidate from the selected repo, might be different than the one from Stable

        (*it)->setIcon(1, QIcon()); // reset update icon
        if (installed.toString().isEmpty()) {
            for (int i = 0; i < newtree->columnCount(); ++i) {
                if (stable_list.contains(app_name)) {
                    (*it)->setToolTip(i, tr("Version ") + stable_list.value(app_name).at(0) + tr(" in stable repo"));
                } else {
                    (*it)->setToolTip(i, tr("Not available in stable repo"));
                }
            }
            (*it)->setText(5, "not installed");
        } else {
            inst_count++;
            if (installed >= repo_candidate) {
                for (int i = 0; i < newtree->columnCount(); ++i) {
                    (*it)->setForeground(2, QBrush(Qt::gray));
                    (*it)->setForeground(4, QBrush(Qt::gray));
                    (*it)->setToolTip(i, tr("Latest version ") + installed.toString() + tr(" already installed"));
                }
                (*it)->setText(5, "installed");
            } else {
                (*it)->setIcon(1, QIcon::fromTheme("software-update-available", QIcon(":/icons/software-update-available.png")));
                for (int i = 0; i < newtree->columnCount(); ++i) {
                    (*it)->setToolTip(i, tr("Version ") + installed.toString() + tr(" installed"));
                }
                upgr_count++;
                (*it)->setText(5, "upgradable");
            }

        }
        ++it;
    }
    bar->setValue(bar->maximum());
    updateInterface();
    newtree->blockSignals(false);
}

void MainWindow::displayFlatpaks(bool force_update)
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";

    setCursor(QCursor(Qt::BusyCursor));
    ui->treeFlatpak->clear();
    ui->treeFlatpak->blockSignals(true);
    change_list.clear();

    if (flatpaks.isEmpty() || force_update) {
        blockInterfaceFP(true);
        flatpaks = listFlatpaks(ui->comboRemote->currentText());
        flatpaks_apps.clear();
        flatpaks_runtimes.clear();

        // list installed packages
        installed_apps_fp = listInstalledFlatpaks("--app");

        // add runtimes (needed for older flatpak versions)
        installed_runtimes_fp = listInstalledFlatpaks("--runtime");
    }

    QStringList installed_all = QStringList() << installed_apps_fp << installed_runtimes_fp;

    int total_count = 0;
    QTreeWidgetItem *widget_item;

    QString short_name, full_name, arch, version, size;
    for (QString item : flatpaks) {
        if (fp_ver < VersionNumber("1.2.4")) {
            size = item.section("\t", 1, 1);
            item = item.section("\t", 0, 0); // strip size
            version = item.section("/", -1);
        } else { // Buster and higher versions
            size = item.section("\t", -1);
            version = item.section("\t", 0, 0);
            item = item.section("\t", 1, 1).section("/", 1);
        }
        full_name = item.section("/", 0, 0);
        short_name = full_name.section(".", -1);
        if (short_name == "Locale" || short_name == "Sources" || short_name == "Debug") { // skip Locale, Sources, Debug
            continue;
        }
        ++total_count;
        widget_item = new QTreeWidgetItem(ui->treeFlatpak);
        widget_item->setCheckState(0, Qt::Unchecked);
        widget_item->setText(1, short_name);
        widget_item->setText(2, full_name);
        widget_item->setText(3, version);
        widget_item->setText(4, size);
        widget_item->setText(8, item); // Full string
        if (installed_all.contains(item)) {
            widget_item->setForeground(1, QBrush(Qt::gray));
            widget_item->setForeground(2, QBrush(Qt::gray));
            widget_item->setText(5, "installed");
        } else {
            widget_item->setText(5, "not installed");
        }
        widget_item->setText(6, "true"); // all items are displayed till filtered
    }

    // add sizes for the installed packages for older flatpak that doesn't list size for all the packages
    listSizeInstalledFP();

    ui->labelNumAppFP->setText(QString::number(total_count));

    int total = 0;
    if (installed_apps_fp != QStringList("")) {
        total = installed_apps_fp.count();
    }
    ui->labelNumInstFP->setText(QString::number(total));

    ui->treeFlatpak->sortByColumn(1, Qt::AscendingOrder);

    removeDuplicatesFP();

    for (int i = 0; i < ui->treeFlatpak->columnCount(); ++i) {
        ui->treeFlatpak->resizeColumnToContents(i);
    }

    ui->treeFlatpak->blockSignals(false);
    filterChanged(ui->comboFilterFlatpak->currentText());
    blockInterfaceFP(false);
    ui->searchBoxFlatpak->setFocus();
}

// Display warning for Debian Backports
void MainWindow::displayWarning(QString repo)
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";

    bool *displayed = nullptr;
    QString msg, file;

    if (repo == "test") {
        displayed = &warning_test;
        file = QDir::homePath() + "/.config/mx-test-installer";
        msg = tr("You are about to use the MX Test repository, whose packages are provided for "\
                 "testing purposes only. It is possible that they might break your system, so it "\
                 "is suggested that you back up your system and install or update only one package "\
                 "at a time. Please provide feedback in the Forum so the package can be evaluated "\
                 "before moving up to Main.");

    } else if (repo == "backports") {
        displayed = &warning_backports;
        file = QDir::homePath() + "/.config/mx-debian-backports-installer";
        msg = tr("You are about to use Debian Backports, which contains packages taken from the next "\
                 "Debian release (called 'testing'), adjusted and recompiled for usage on Debian stable. "\
                 "They cannot be tested as extensively as in the stable releases of Debian and MX Linux, "\
                 "and are provided on an as-is basis, with risk of incompatibilities with other components "\
                 "in Debian stable. Use with care!");
    } else if (repo == "flatpaks") {
        displayed = &warning_flatpaks;
        file = QDir::homePath() + "/.config/mxpi_nowarning_flatpaks";
        msg = tr("MX Linux includes this repository of flatpaks for the users' convenience only, and "\
                 "is not responsible for the functionality of the individual flatpaks themselves. "\
                 "For more, consult flatpaks in the Wiki.");
    }

    if (*displayed || QFileInfo::exists(file)) {
        return;
    }

    QMessageBox msgBox(QMessageBox::Warning, tr("Warning"), msg , nullptr, nullptr);
    msgBox.addButton(QMessageBox::Close);
    QCheckBox *cb = new QCheckBox();
    msgBox.setCheckBox(cb);
    cb->setText(tr("Do not show this message again"));
    connect(cb, &QCheckBox::clicked, [this, &file, &cb] () {disableWarning(cb->isChecked(), file);});
    msgBox.exec();
    *displayed = true;
}

// If dowload fails hide progress bar and show first tab
void MainWindow::ifDownloadFailed()
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";
    progress->hide();
    ui->tabWidget->setCurrentWidget(ui->tabPopular);
}


// List the flatpak remote and loade them into combobox
void MainWindow::listFlatpakRemotes()
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";
    ui->comboRemote->blockSignals(true);
    ui->comboRemote->clear();
    QStringList list = cmd.getCmdOut("runuser -l $(logname) -c \"flatpak remote-list " +  user + "| cut -f1\"").remove(" ").split("\n");
    ui->comboRemote->addItems(list);
    //set flathub default
    ui->comboRemote->setCurrentIndex(ui->comboRemote->findText("flathub"));
    ui->comboRemote->blockSignals(false);
}

// Display warning for Debian Backports
bool MainWindow::confirmActions(QString names, QString action)
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";
    qDebug() << "names" << names << "and" << change_list;
    QString msg;

    QString detailed_names;
    QStringList detailed_installed_names;
    QString detailed_to_install;
    QString detailed_removed_names;
    QString recommends;
    if (tree == ui->treeFlatpak){
        detailed_installed_names = change_list;
    } else if (tree == ui->treeBackports) {
        recommends = (ui->checkBoxInstallRecommendsMXBP->isChecked()) ? "--install-recommends " : "";
        detailed_names = cmd.getCmdOut("DEBIAN_FRONTEND=$(xprop -root | grep -sqi kde && echo kde || echo gnome) LANG=C apt-get -s -V -o=Dpkg::Use-Pty=0 " + action + " " + recommends + "-t " + ver_name + "-backports --reinstall " + names + "|grep 'Inst\\|Remv' | awk '{print $2 \";\" $3 \";\" $1}'");
    } else if (tree == ui->treeMXtest) {
        recommends = (ui->checkBoxInstallRecommendsMX->isChecked()) ? "--install-recommends " : "";
        detailed_names = cmd.getCmdOut("DEBIAN_FRONTEND=$(xprop -root | grep -sqi kde && echo kde || echo gnome) LANG=C apt-get -s -V -o=Dpkg::Use-Pty=0 " + action + " -t a=mx,c=test " + recommends +  "--reinstall " + names + "|grep 'Inst\\|Remv' | awk '{print $2 \";\" $3 \";\" $1}'");
    } else {
        recommends = (ui->checkBoxInstallRecommends->isChecked()) ? "--install-recommends " : "";
        detailed_names = cmd.getCmdOut("DEBIAN_FRONTEND=$(xprop -root | grep -sqi kde && echo kde || echo gnome) LANG=C apt-get -s -V -o=Dpkg::Use-Pty=0 " + action + " " + recommends +  "--reinstall " + names + "|grep 'Inst\\|Remv'| awk '{print $2 \";\" $3 \";\" $1}'");
    }

    if (tree != ui->treeFlatpak){
        detailed_installed_names=detailed_names.split("\n");
    }

    detailed_installed_names.sort();
    qDebug() << "detailed installed names sorted " << detailed_installed_names;
    QStringListIterator iterator(detailed_installed_names);
    if (tree != ui->treeFlatpak){
        while (iterator.hasNext()){
            QString value = iterator.next();
            if (value.contains("Remv")){
                value = value.section(";",0,0) + " " + value.section(";",1,1);
                detailed_removed_names = detailed_removed_names + value + "\n";
            }
            if (value.contains("Inst")){
                value = value.section(";",0,0) + " " + value.section(";",1,1) + ")";
                detailed_to_install = detailed_to_install + value + "\n";
            }
        }
        if (!detailed_removed_names.isEmpty()){
            detailed_removed_names.prepend(tr("Remove") + "\n");
        }
        if (!detailed_to_install.isEmpty())
            detailed_to_install.prepend(tr("Install") + "\n");
    } else {
        if (action == "remove") {
            detailed_removed_names = change_list.join("\n");
            detailed_to_install.clear();        }
        if (action == "install") {
            detailed_to_install = change_list.join("\n");
            detailed_removed_names.clear();
        }
    }

        msg = "<b>" + tr("The following packages were selected. Click Show Details for list of changes.") + "</b>";

        QMessageBox msgBox;
        msgBox.setText(msg);
        msgBox.setInformativeText("\n" + names);
        msgBox.addButton(QMessageBox::Ok);
        msgBox.addButton(QMessageBox::Cancel);
        if (action == "install"){
            //msgBox.setDetailedText(tr("Install") + "\n" + detailed_names + "\n" + tr("Remove") + "\n" + detailed_names);
            msgBox.setDetailedText(detailed_to_install + "\n" + detailed_removed_names);
        } else {
            msgBox.setDetailedText(detailed_removed_names + "\n" + detailed_to_install);
        }
        msgBox.setFixedWidth(700);
        return msgBox.exec() == QMessageBox::Ok;
}

// Install the list of apps
bool MainWindow::install(const QString &names)
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";
    bar->setValue(0);

    if (!checkOnline()) {
        QMessageBox::critical(this, tr("Error"), tr("Internet is not available, won't be able to download the list of packages"));
        return false;
    }

    lock_file->unlock();
    ui->tabWidget->setTabText(ui->tabWidget->indexOf(ui->tabOutput), tr("Installing packages..."));

    bool success = false;

    QString recommends;

    //simulate install of selections and present for confirmation
    //if user selects cancel, break routine but return success to avoid error message
    if (!confirmActions(names, "install")) return true;

    displayOutput();
    if (tree == ui->treeBackports) {
        recommends = (ui->checkBoxInstallRecommendsMXBP->isChecked()) ? "--install-recommends " : "";
        success = cmd.run("DEBIAN_FRONTEND=$(xprop -root | grep -sqi kde && echo kde || echo gnome) apt-get -o=Dpkg::Use-Pty=0 install -y " + recommends +  "-t " + ver_name + "-backports --reinstall " + names);
    } else if (tree == ui->treeMXtest) {
        recommends = (ui->checkBoxInstallRecommendsMX->isChecked()) ? "--install-recommends " : "";
        success = cmd.run("DEBIAN_FRONTEND=$(xprop -root | grep -sqi kde && echo kde || echo gnome) apt-get -o=Dpkg::Use-Pty=0 install -y " + recommends +  "-t a=mx,c=test " + names);
    } else {
        recommends = (ui->checkBoxInstallRecommends->isChecked()) ? "--install-recommends " : "";
        success = cmd.run("DEBIAN_FRONTEND=$(xprop -root | grep -sqi kde && echo kde || echo gnome) apt-get -o=Dpkg::Use-Pty=0 install -y " + recommends +  "--reinstall " + names);
    }
    lock_file->lock();
    bar->setValue(bar->maximum());

    return success;
}

// install a list of application and run postprocess for each of them.
bool MainWindow::installBatch(const QStringList &name_list)
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";
    QString postinstall;
    QString install_names;
    bool result = true;

    // load all the
    for (const QString &name : name_list) {
        for (const QStringList &list : popular_apps) {
            if (list.at(1) == name) {
                postinstall += list.at(6) + "\n";
                install_names += list.at(7) + " ";
            }
        }
    }

    if (!install_names.isEmpty()) {
        if (!install(install_names)) {
            result = false;
        }
    }
    if (postinstall != "\n") {
        qDebug() << "Post-install";
        ui->tabWidget->setTabText(ui->tabWidget->indexOf(ui->tabOutput), tr("Post-processing..."));
        lock_file->unlock();
        displayOutput();
        if (!cmd.run(postinstall)) {
            result = false;
        }
    }
    lock_file->lock();
    return result;
}

// install named app
bool MainWindow::installPopularApp(const QString &name)
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";
    int result = true;
    QString preinstall;
    QString postinstall;
    QString install_names;

    // get all the app info
    for (const QStringList &list : popular_apps) {
        if (list.at(1) == name) {
            preinstall = list.at(5);
            postinstall = list.at(6);
            install_names = list.at(7);
        }
    }
    displayOutput();
    // preinstall
    if (!preinstall.isEmpty()) {
        qDebug() << "Pre-install";
        ui->tabWidget->setTabText(ui->tabWidget->indexOf(ui->tabOutput), tr("Pre-processing for ") + name);
        lock_file->unlock();
        if (!cmd.run(preinstall)) {
            QFile file("/etc/apt/sources.list.d/mxpitemp.list"); // remove temp source list if it exists
            if (file.exists()) {
                file.remove();
                update();
            }
            return false;
        }
    }
    // install
    if (!install_names.isEmpty()) {
        ui->tabWidget->setTabText(ui->tabWidget->indexOf(ui->tabOutput), tr("Installing ") + name);
        result = install(install_names);
    }
    displayOutput();
    // postinstall
    if (!postinstall.isEmpty()) {
        qDebug() << "Post-install";
        ui->tabWidget->setTabText(ui->tabWidget->indexOf(ui->tabOutput), tr("Post-processing for ") + name);
        lock_file->unlock();
        cmd.run(postinstall);
    }
    lock_file->lock();
    return result;
}


// Process checked items to install
bool MainWindow::installPopularApps()
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";

    QStringList batch_names;
    bool result = true;

    if (!checkOnline()) {
        QMessageBox::critical(this, tr("Error"), tr("Internet is not available, won't be able to download the list of packages"));
        return false;
    }
    if (!updated_once) {
        update();
    }

    // make a list of apps to be installed together
    QTreeWidgetItemIterator it(ui->treePopularApps);
    while (*it) {
        if ((*it)->checkState(1) == Qt::Checked) {
            QString name = (*it)->text(2);
            for (const QStringList &list : popular_apps) {
                if (list.at(1) == name) {
                    QString preinstall = list.at(5);
                    if (preinstall.isEmpty()) {  // add to batch processing if there is not preinstall command
                        batch_names << name;
                        (*it)->setCheckState(1, Qt::Unchecked);
                    }
                }
            }
        }
        ++it;
    }
    if (!installBatch(batch_names)) {
        result = false;
    }

    // install the rest of the apps
    QTreeWidgetItemIterator iter(ui->treePopularApps);
    while (*iter) {
        if ((*iter)->checkState(1) == Qt::Checked) {
            if (!installPopularApp((*iter)->text(2))) {
                result = false;
            }
        }
        ++iter;
    }
    setCursor(QCursor(Qt::ArrowCursor));
    return result;
}

// Install selected items
bool MainWindow::installSelected()
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";
    ui->tabWidget->setTabEnabled(ui->tabWidget->indexOf(ui->tabOutput), true);
    QString names = change_list.join(" ");

    // change sources as needed
    if(tree == ui->treeMXtest) {
        if (cmd.run("grep -q '^#\\s*deb.* test' /etc/apt/sources.list.d/mx.list")) { // commented out line
            cmd.run("sed -i '/^#*\\s*deb.* test/s/^#*//' /etc/apt/sources.list.d/mx.list"); // uncomment
        } else { // doesn't exist, add
            if (ver_name == "jessie") { // use 'mx15' for Stretch based MX, user version name for newer versions
                cmd.run("echo -e '\ndeb http://mxrepo.com/mx/testrepo/ mx15 test' >> /etc/apt/sources.list.d/mx.list");
            } else {
                cmd.run("echo -e '\ndeb http://mxrepo.com/mx/testrepo/ " + ver_name + " test' >> /etc/apt/sources.list.d/mx.list");
            }
        }
        update();
    } else if (tree == ui->treeBackports) {
        cmd.run("echo deb http://ftp.debian.org/debian " + ver_name + "-backports main contrib non-free>/etc/apt/sources.list.d/mxpm-temp.list");
        update();
    }
    progress->hide();
    bool result = install(names);
    if (tree == ui->treeBackports) {
        cmd.run("rm -f /etc/apt/sources.list.d/mxpm-temp.list");
        update();
    } else if (tree == ui->treeMXtest && !test_initially_enabled) {
        cmd.run("sed -i 's/.* test/#&/'  /etc/apt/sources.list.d/mx.list");  // comment out the line
        update();
    }
    change_list.clear();
    installed_packages = listInstalled();
    return result;
}


// check if the name is filtered (lib, dev, dbg, etc.)
bool MainWindow::isFilteredName(const QString &name) const
{
    return ((name.startsWith("lib") && !name.startsWith("libreoffice")) || name.endsWith("-dev") || name.endsWith("-dbg") || name.endsWith("-dbgsym"));
}

// Check if online
bool MainWindow::checkOnline() const
{
    return(system("wget -q --spider http://mxrepo.com >/dev/null 2>&1 || wget -q --spider http://google.com >/dev/null 2>&1 ") == 0);
}

// Build the list of available packages from various source
bool MainWindow::buildPackageLists(bool force_download)
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";
    clearUi();
    if (!downloadPackageList(force_download)) {
        ifDownloadFailed();
        return false;
    }
    if (!readPackageList(force_download)) {
        ifDownloadFailed();
        return false;
    }
    displayPackages();
    return true;
}

// Download the Packages.gz from sources
bool MainWindow::downloadPackageList(bool force_download)
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";
    QString repo_name;

    if (!checkOnline()) {
        QMessageBox::critical(this, tr("Error"), tr("Internet is not available, won't be able to download the list of packages"));
        return false;
    }
    if (tmp_dir.isEmpty()) {
        tmp_dir = cmd.getCmdOut("mktemp -d /tmp/mxpm-XXXXXXXX");
    }
    QDir::setCurrent(tmp_dir);
    progress->setLabelText(tr("Downloading package info..."));
    progCancel->setEnabled(true);

    if (stable_list.isEmpty() || force_download) {
        if (force_download) {
            progress->show();
            if (!update()) {
                return false;
            }
        }
        progress->show();
        AptCache cache;
        stable_list = cache.getCandidates();
        if (stable_list.isEmpty()) {
            update();
            AptCache cache;
            stable_list = cache.getCandidates();
        }
    }

    if (tree == ui->treeMXtest)  {
        if (!QFile(tmp_dir + "/mxPackages").exists() || force_download) {
            progress->show();

            if (ver_name == "jessie") { // repo name is 'mx15' for Strech, use Debian version name for later versions
                repo_name = "mx15";
            } else {
                repo_name = ver_name;
            }
            if (!cmd.run("wget --append-output=/var/log/mxpi.log http://mxrepo.com/mx/testrepo/dists/" + repo_name + "/test/binary-" + arch +
                         "/Packages.gz -O mxPackages.gz && gzip -df mxPackages.gz")) {
                QFile::remove(tmp_dir + "/mxPackages.gz");
                QFile::remove(tmp_dir + "/mxPackages");
                return false;
            }
        }

    } else if (tree == ui->treeBackports) {
        if (!QFile(tmp_dir + "/mainPackages").exists() ||
                !QFile(tmp_dir + "/contribPackages").exists() ||
                !QFile(tmp_dir + "/nonfreePackages").exists() || force_download) {
            progress->show();
            bool success = cmd.run("wget --append-output=/var/log/mxpi.log --timeout=10 http://deb.debian.org/debian/dists/" +
                               ver_name + "-backports/main/binary-" + arch + "/Packages.xz -O mainPackages.xz && unxz -f mainPackages.xz");
            if (!success) {
                QFile::remove(tmp_dir + "/mainPackages.xz");
                QFile::remove(tmp_dir + "/mainPackages");
                return false;
            }
            //cmd.run("sleep 3");
            success = cmd.run("wget --append-output=/var/log/mxpi.log --timeout=10 http://deb.debian.org/debian/dists/" +
                           ver_name + "-backports/contrib/binary-" + arch + "/Packages.xz -O contribPackages.xz && unxz -f contribPackages.xz");
            if (!success) {
                QFile::remove(tmp_dir + "/contribPackages.xz");
                QFile::remove(tmp_dir + "/contribPackages");
                return false;
            }
            //cmd.run("sleep 3");
            success = cmd.run("wget --append-output=/var/log/mxpi.log --timeout=10 http://deb.debian.org/debian/dists/" +
                           ver_name + "-backports/non-free/binary-" + arch + "/Packages.xz -O nonfreePackages.xz && unxz -f nonfreePackages.xz");
            if (!success) {
                QFile::remove(tmp_dir + "/nonfreePackages.xz");
                QFile::remove(tmp_dir + "/nonfreePackages");
                return false;
            }
            progCancel->setDisabled(true);
            cmd.run("cat mainPackages contribPackages nonfreePackages > allPackages");
        }
    }
    return true;
}

void MainWindow::enableTabs(bool enable)
{
    for (int tab = 0; tab < 5; ++tab) {
        ui->tabWidget->setTabEnabled(tab, enable);
    }
}

// Process downloaded *Packages.gz files
bool MainWindow::readPackageList(bool force_download)
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";
    progCancel->setDisabled(true);
    // don't process if the lists are already populated
    if (!((tree == ui->treeStable && stable_list.isEmpty()) || (tree == ui->treeMXtest && mx_list.isEmpty())||
          (tree == ui->treeBackports && backports_list.isEmpty()) || force_download)) {
        return true;
    }

    QFile file;
    if (tree == ui->treeMXtest)  { // read MX Test list
        file.setFileName(tmp_dir + "/mxPackages");
        if(!file.open(QFile::ReadOnly)) {
            qDebug() << "Could not open file: " << file.fileName();
            return false;
        }
    } else if (tree == ui->treeBackports) {  // read Backports lsit
        file.setFileName(tmp_dir + "/allPackages");
        if(!file.open(QFile::ReadOnly)) {
            qDebug() << "Could not open file: " << file.fileName();
            return false;
        }
    }

    QString file_content = file.readAll();
    file.close();

    QMap<QString, QStringList> map;
    QStringList package_list;
    QStringList version_list;
    QStringList description_list;

    const QStringList list = file_content.split("\n");

    for (QString line : list) {
        if (line.startsWith("Package: ")) {
            package_list << line.remove("Package: ");
        } else if (line.startsWith("Version: ")) {
            version_list << line.remove("Version: ");
        } else if (line.startsWith("Description: ")) {
            description_list << line.remove("Description: ");
        }
    }

    for (int i = 0; i < package_list.size(); ++i) {
        map.insert(package_list.at(i), QStringList() << version_list.at(i) << description_list.at(i));
    }

    if (tree == ui->treeMXtest)  {
        mx_list = map;
    } else if (tree == ui->treeBackports) {
        backports_list = map;
    }

    return true;
}

// Cancel download
void MainWindow::cancelDownload()
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";
    cmd.terminate();
}

// Clear UI when building package list
void MainWindow::clearUi()
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";

    blockSignals(true);
    ui->comboFilterStable->setCurrentIndex(0);
    ui->comboFilterMX->setCurrentIndex(0);
    ui->comboFilterBP->setCurrentIndex(0);

    ui->buttonCancel->setEnabled(true);
    ui->buttonInstall->setEnabled(false);
    ui->buttonUninstall->setEnabled(false);

    if (tree == ui->treeStable || tree == ui->treePopularApps) {
        ui->labelNumApps->clear();
        ui->labelNumInst->clear();
        ui->labelNumUpgr->clear();
        ui->treeStable->clear();
        ui->buttonUpgradeAll->setHidden(true);
    } else if (tree == ui->treeMXtest || tree == ui->treePopularApps) {
        ui->labelNumApps_2->clear();
        ui->labelNumInstMX->clear();
        ui->labelNumUpgrMX->clear();
        ui->treeMXtest->clear();
    } else if (tree == ui->treeBackports || tree == ui->treePopularApps) {
        ui->labelNumApps_3->clear();
        ui->labelNumInstBP->clear();
        ui->labelNumUpgrBP->clear();
        ui->treeBackports->clear();
    }
    blockSignals(false);
}

// Copy QTreeWidgets
void MainWindow::copyTree(QTreeWidget *from, QTreeWidget *to) const
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";
    to->clear();
    QTreeWidgetItem *item;
    QTreeWidgetItemIterator it(from);
    while (*it) {
        item = new QTreeWidgetItem();
        item = (*it)->clone();
        to->addTopLevelItem(item);
        ++it;
    }
}

// Cleanup environment when window is closed
void MainWindow::cleanup()
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";

    cmd.halt();

    bool changed = false;
    if (QFile::exists("/etc/apt/sources.list.d/mxpm-temp.list")) {
        system("rm -f /etc/apt/sources.list.d/mxpm-temp.list");
        changed = true;
    }
    if (!test_initially_enabled && (system("grep -q '^\\s*deb.* test' /etc/apt/sources.list.d/mx.list") == 0)) {
        system("sed -i 's/.* test/#&/'  /etc/apt/sources.list.d/mx.list");  // comment out the line
        changed = true;
    }
    if (changed) {
        update();
    }

    lock_file->unlock();

    QDir::setCurrent("/");
    if (tmp_dir.startsWith("/tmp/mxpm-") && QFile::exists(tmp_dir)) {
        system("rm -r " + tmp_dir.toUtf8());
    }

    QSettings settings("mx-packageinstaller");
    settings.setValue("geometry", saveGeometry());
}

// Get version of the program
QString MainWindow::getVersion(const QString name)
{
    return cmd.getCmdOut("dpkg-query -f '${Version}' -W " + name);
}

// Return true if all the packages listed are installed
bool MainWindow::checkInstalled(const QString &names) const
{
    if (names.isEmpty()) {
        return false;
    }
    for (const QString &name : names.split("\n")) {
        if (!installed_packages.contains(name.trimmed())) {
            return false;
        }
    }
    return true;
}

// Return true if all the packages in the list are installed
bool MainWindow::checkInstalled(const QStringList &name_list) const
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";
    if (name_list.size() == 0) {
        return false;
    }
    for (const QString &name : name_list) {
        if (!installed_packages.contains(name)) {
            return false;
        }
    }
    return true;
}

// return true if all the items in the list are upgradable
bool MainWindow::checkUpgradable(const QStringList &name_list) const
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";
    if (name_list.size() == 0) {
        return false;
    }
    QList<QTreeWidgetItem *> item_list;
    for (const QString &name : name_list) {
        item_list = tree->findItems(name, Qt::MatchExactly, 2);
        if (item_list.isEmpty()) {
            return false;
        }
        if (item_list.at(0)->text(5) != "upgradable") {
            return false;
        }
    }
    return true;
}


// Returns list of all installed packages
QStringList MainWindow::listInstalled()
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";
    disconnect(conn);
    QString str = cmd.getCmdOut("dpkg --get-selections | grep -v deinstall | cut -f1");
    conn = connect(&cmd, &Cmd::outputAvailable, [](const QString &out) { qDebug() << out.trimmed(); });
    str.remove(":i386");
    str.remove(":amd64");
    return str.split("\n");
}


// Return list flatpaks from current remote
QStringList MainWindow::listFlatpaks(const QString remote, const QString type)
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";
    static bool updated = false;

    bool success = false;
    QString out;
    QStringList list;
    // need to specify arch for older version
    QString arch_fp;
    disconnect(conn);
    if (fp_ver < VersionNumber("1.0.1")) {
        arch_fp = (arch == "amd64") ? arch_fp = "--arch=x86_64 " : arch_fp = "--arch=i386 ";
        // list packages, strip first part remote/ or app/ no size for old flatpak
        success = cmd.run("runuser -l $(logname) -c \"set -o pipefail; flatpak -d remote-ls " + user + remote + " " + arch_fp + type + "| cut -f1 | tr -s ' ' | cut -f1 -d' '|sed 's/^[^\\/]*\\///g' \"", out);
        list = QString(out).split("\n");
    } else if (fp_ver < VersionNumber("1.2.4")) { // lower than Buster version
        // list size too
        success = cmd.run("runuser -l $(logname) -c \"set -o pipefail; flatpak -d remote-ls " + user + remote + " " + arch_fp + type + "| cut -f1,3 |tr -s ' ' | sed 's/^[^\\/]*\\///g' \"", out);
        list = QString(out).split("\n");
    } else { // Buster version and above
        if (!updated) {
            success = cmd.run("runuser -l $(logname) -c \"flatpak update --appstream\"");
            updated = true;
        }
        // list version too
        QString process_string; // unfortunatelly the resulting string structure is different depending on type option
        if (type == "--app" || type.isEmpty()) {
            success = cmd.run("runuser -l $(logname) -c \"set -o pipefail; flatpak remote-ls " + user + remote + " " + arch_fp + "--app --columns=ver,ref,installed-size\"", out);
            list = QString(out).split("\n");
            if (list == QStringList("")) list = QStringList();
        }
        if (type == "--runtime" || type.isEmpty()) {
            success = cmd.run("runuser -l $(logname) -c \"set -o pipefail; flatpak remote-ls " + user + remote + " " + arch_fp + "--runtime --columns=branch,ref,installed-size\"", out);
            list += QString(out).split("\n");
            if (list == QStringList("")) list = QStringList();
        }
    }
    conn = connect(&cmd, &Cmd::outputAvailable, [](const QString &out) { qDebug() << out.trimmed(); });

    if (!success || list == QStringList("")) {
        qDebug() << QString("Could not list packages from %1 remote, or remote doesn't contain packages").arg(remote);
        return QStringList();
    }
    return list;
}

// list installed flatpaks by type: apps, runtimes, or all (if no type is provided)
QStringList MainWindow::listInstalledFlatpaks(const QString type)
{
    QStringList list;
    if (fp_ver < VersionNumber("1.2.4")) {
        list << cmd.getCmdOut("runuser -l $(logname) -c \"flatpak -d list " + user + type + "|cut -f1|cut -f1 -d' '\"").remove(" ").split("\n");
    } else {
        list << cmd.getCmdOut("runuser -l $(logname) -c \"flatpak list " + user + type + " --columns=ref\"").remove(" ").split("\n");
    }
    if (list == QStringList("")) list = QStringList();
    return list;
}


// return the visible tree
void MainWindow::setCurrentTree()
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";
    const QList<QTreeWidget *> list({ui->treePopularApps, ui->treeStable, ui->treeMXtest, ui->treeBackports, ui->treeFlatpak});

    for (QTreeWidget *item : list) {
        if (item->isVisible()) {
            tree = item;
            return;
        }
    }
}

QHash<QString, VersionNumber> MainWindow::listInstalledVersions()
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";
    disconnect(conn);
    QString out = cmd.getCmdOut("dpkg -l | grep '^ii'", true);
    conn = connect(&cmd, &Cmd::outputAvailable, [](const QString &out) { qDebug() << out.trimmed(); });

    QString name;
    QString ver_str;
    QStringList item;
    QHash<QString, VersionNumber> result;

    const QStringList list = out.split("\n");
    for (const QString &line : list) {
        item = line.split(QRegularExpression("\\s{2,}"));
        name = item.at(1);
        name.remove(":i386").remove(":amd64");
        ver_str = item.at(2);
        ver_str.remove(" amd64");
        result.insert(name, VersionNumber(ver_str));
    }
    return result;
}


// Things to do when the command starts
void MainWindow::cmdStart()
{
    timer.start(100);
    setCursor(QCursor(Qt::BusyCursor));
    ui->lineEdit->setFocus();
}


// Things to do when the command is done
void MainWindow::cmdDone()
{
    timer.stop();
    setCursor(QCursor(Qt::ArrowCursor));
    disableOutput();
    bar->setValue(bar->maximum());
}

void MainWindow::displayOutput()
{
    connect(&cmd, &Cmd::outputAvailable, this, &MainWindow::outputAvailable, Qt::UniqueConnection);
    connect(&cmd, &Cmd::errorAvailable, this, &MainWindow::outputAvailable, Qt::UniqueConnection);
}

void MainWindow::disableOutput()
{
    disconnect(&cmd, &Cmd::outputAvailable, this, &MainWindow::outputAvailable);
    disconnect(&cmd, &Cmd::errorAvailable, this, &MainWindow::outputAvailable);
}

// Disable Backports warning
void MainWindow::disableWarning(bool checked, QString file)
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";
    if (checked) {
        system("touch " + file.toUtf8());
    } else {
        system("rm " + file.toUtf8());
    }
}

// Display info when clicking the "info" icon of the package
void MainWindow::displayInfo(const QTreeWidgetItem *item, int column)
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";
    if (column == 3 && item->childCount() == 0) {
        QString desc = item->text(4);
        QString install_names = item->text(5);
        QString title = item->text(2);
        QString msg = "<b>" + title + "</b><p>" + desc + "<p>" ;
        if (install_names != "") {
            msg += tr("Packages to be installed: ") + install_names;
        }
        QUrl url = item->text(7); // screenshot url

        if (!url.isValid() || url.isEmpty() || url.url() == "none") {
            qDebug() << "no screenshot for: " << title;
        } else {
            QNetworkAccessManager *manager = new QNetworkAccessManager(this);
            QNetworkReply *reply = manager->get(QNetworkRequest(url));

            QEventLoop loop;
            connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
            QTimer timer;
            timer.start(5000);
            connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
            ui->treePopularApps->blockSignals(true);
            loop.exec();
            timer.stop();
            ui->treePopularApps->blockSignals(false);

            if (reply->error())
            {
                qDebug() << "Download of " << url.url() << " failed: " << qPrintable(reply->errorString());
            } else {
                QImage image;
                QByteArray data;
                QBuffer buffer(&data);
                QImageReader imageReader(reply);
                image = imageReader.read();
                if (imageReader.error()) {
                    qDebug() << "loading screenshot: " << imageReader.errorString();
                } else {
                    image = image.scaled(QSize(200,300), Qt::KeepAspectRatioByExpanding);
                    image.save(&buffer, "PNG");
                    msg += QString("<p><img src='data:image/png;base64, %0'>").arg(QString(data.toBase64()));
                }
            }
        }
        QMessageBox info(QMessageBox::NoIcon, tr("Package info") , msg, QMessageBox::Close, this);
        info.exec();
    }
}

// Find package in view
void MainWindow::findPopular() const
{
    QTreeWidgetItemIterator it(ui->treePopularApps);
    QString word = ui->searchPopular->text();
    if (word.length() == 1) {
        return;
    }
    if (word.isEmpty()) {
        while (*it) {
            (*it)->setExpanded(false);
            ++it;
        }
        ui->treePopularApps->reset();
        for (int i = 0; i < 5; ++i) {
            ui->treePopularApps->resizeColumnToContents(i);
        }
        return;
    }
    QList<QTreeWidgetItem *> found_items = ui->treePopularApps->findItems(word, Qt::MatchContains|Qt::MatchRecursive, 2);
    found_items << ui->treePopularApps->findItems(word, Qt::MatchContains|Qt::MatchRecursive, 4);

    // hide/unhide items
    while (*it) {
        if ((*it)->childCount() == 0) { // if child
            if (found_items.contains(*it)) {
                (*it)->setHidden(false);
            } else {
                (*it)->parent()->setHidden(true);
                (*it)->setHidden(true);
            }
        }
        ++it;
    }

    // process found items
    for (QTreeWidgetItem* item : found_items) {
        if (item->childCount() == 0) { // if child, expand parent
            item->parent()->setExpanded(true);
            item->parent()->setHidden(false);
        } else {  // if parent, expand children
            item->setExpanded(true);
            item->setHidden(false);
            int count = item->childCount();
            for (int i = 0; i < count; ++i) {
                item->child(i)->setHidden(false);
            }
        }
    }
    for (int i = 0; i < 5; ++i) {
        ui->treePopularApps->resizeColumnToContents(i);
    }
}

// Find packages in other sources
void MainWindow::findPackageOther()
{
    QString word;
    if (tree == ui->treeStable) {
        word = ui->searchBoxStable->text();
    } else if (tree == ui->treeMXtest) {
        word = ui->searchBoxMX->text();
    } else if (tree == ui->treeBackports) {
        word = ui->searchBoxBP->text();
    } else if (tree == ui->treeFlatpak) {
        word = ui->searchBoxFlatpak->text();
    }
    if (word.length() == 1) {
        return;
    }

    QList<QTreeWidgetItem *> found_items = tree->findItems(word, Qt::MatchContains, 2);
    if (tree != ui->treeFlatpak) { // treeFlatpak has a different column structure
        found_items << tree->findItems(word, Qt::MatchContains, 4);
    }
    QTreeWidgetItemIterator it(tree);
    while (*it) {
        if ((*it)->text(6) == "true" && found_items.contains(*it)) {
            (*it)->setHidden(false);
        } else {
            (*it)->setHidden(true);
        }
        // hide libs
        QString app_name = (*it)->text(2);
        if (isFilteredName(app_name) && ui->checkHideLibs->isChecked()) {
            (*it)->setHidden(true);
        }
        ++it;
    }
}

void MainWindow::showOutput()
{
    ui->outputBox->clear();
    ui->tabWidget->setTabEnabled(ui->tabWidget->indexOf(ui->tabOutput), true);
    ui->tabWidget->setCurrentWidget(ui->tabOutput);
    enableTabs(false);
}

// Install button clicked
void MainWindow::on_buttonInstall_clicked()
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";
    //qDebug() << "change list"  << .join(" ");
    showOutput();

    if (tree == ui->treePopularApps) {
        bool success = installPopularApps();
        if(stable_list.size() > 0) { // clear cache to update list if it already exists
            buildPackageLists();
        }
        if (success) {
            refreshPopularApps();
            QMessageBox::information(this, tr("Done"), tr("Processing finished successfully."));
            ui->tabWidget->setCurrentWidget(tree->parentWidget());
        } else {
            refreshPopularApps();
            QMessageBox::critical(this, tr("Error"), tr("Problem detected while installing, please inspect the console output."));
        }
    } else if (tree == ui->treeFlatpak) {
        //confirmation dialog
        if (!confirmActions(change_list.join(" "), "install")) {
            displayFlatpaks(true);
            indexFilterFP.clear();
            ui->comboFilterFlatpak->setCurrentIndex(0);
            QMessageBox::information(this, tr("Done"), tr("Processing finished successfully."));
            ui->tabWidget->blockSignals(true);
            ui->tabWidget->setCurrentWidget(ui->tabFlatpak);
            ui->tabWidget->blockSignals(false);
            enableTabs(true);
            return;
        }
        setCursor(QCursor(Qt::BusyCursor));
        displayOutput();
        if (cmd.run("runuser -l $(logname) -c \"socat SYSTEM:'flatpak install -y " + user + ui->comboRemote->currentText() + " " + change_list.join(" ") + "',stderr STDIO\"")) {
            displayFlatpaks(true);
            indexFilterFP.clear();
            ui->comboFilterFlatpak->setCurrentIndex(0);
            QMessageBox::information(this, tr("Done"), tr("Processing finished successfully."));
            ui->tabWidget->blockSignals(true);
            ui->tabWidget->setCurrentWidget(ui->tabFlatpak);
            ui->tabWidget->blockSignals(false);
        } else {
            setCursor(QCursor(Qt::ArrowCursor));
            QMessageBox::critical(this, tr("Error"), tr("Problem detected while installing, please inspect the console output."));
        }
    } else {
        bool success = installSelected();
        buildPackageLists();
        refreshPopularApps();
        if (success) {
            QMessageBox::information(this, tr("Done"), tr("Processing finished successfully."));
            ui->tabWidget->setCurrentWidget(tree->parentWidget());
        } else {
            QMessageBox::critical(this, tr("Error"), tr("Problem detected while installing, please inspect the console output."));
        }
    }
    enableTabs(true);
}

// About button clicked
void MainWindow::on_buttonAbout_clicked()
{
    this->hide();
    displayAboutMsgBox(tr("About %1").arg(this->windowTitle()), "<p align=\"center\"><b><h2>" + this->windowTitle() +"</h2></b></p><p align=\"center\">" +
                       tr("Version: ") + VERSION + "</p><p align=\"center\"><h3>" +
                       tr("Package Installer for MX Linux") +
                       "</h3></p><p align=\"center\"><a href=\"http://mxlinux.org\">http://mxlinux.org</a><br /></p><p align=\"center\">" +
                       tr("Copyright (c) MX Linux") + "<br /><br /></p>",
                       "/usr/share/doc/mx-packageinstaller/license.html", tr("%1 License").arg(this->windowTitle()), true);
    this->show();
}
// Help button clicked
void MainWindow::on_buttonHelp_clicked()
{
    QLocale locale;
    QString lang = locale.bcp47Name();

    QString url = "/usr/share/doc/mx-packageinstaller/help/mx-package-installer.html";

    if (lang.startsWith("fr")) {
        url = "https://mxlinux.org/wiki/help-files/help-mx-installateur-de-paquets";
    }
    displayDoc(url, tr("%1 Help").arg(this->windowTitle()), true);
}

// Resize columns when expanding
void MainWindow::on_treePopularApps_expanded()
{
    ui->treePopularApps->resizeColumnToContents(2);
    ui->treePopularApps->resizeColumnToContents(4);
}

// Tree item clicked
void MainWindow::on_treePopularApps_itemClicked()
{
    bool checked = false;
    bool installed = true;

    QTreeWidgetItemIterator it(ui->treePopularApps);
    while (*it) {
        if ((*it)->checkState(1) == Qt::Checked) {
            checked = true;
            if ((*it)->foreground(2) != Qt::gray) {
                installed = false;
            }
        }
        ++it;
    }
    ui->buttonInstall->setEnabled(checked);
    ui->buttonUninstall->setEnabled(checked && installed);
    if (checked && installed) {
        ui->buttonInstall->setText(tr("Reinstall"));
    } else {
        ui->buttonInstall->setText(tr("Install"));
    }
}

// Tree item expanded
void MainWindow::on_treePopularApps_itemExpanded(QTreeWidgetItem *item)
{
    item->setIcon(0, QIcon::fromTheme("folder-open"));
    ui->treePopularApps->resizeColumnToContents(2);
    ui->treePopularApps->resizeColumnToContents(4);
}

// Tree item collapsed
void MainWindow::on_treePopularApps_itemCollapsed(QTreeWidgetItem *item)
{
    item->setIcon(0, QIcon::fromTheme("folder"));
    ui->treePopularApps->resizeColumnToContents(2);
    ui->treePopularApps->resizeColumnToContents(4);
}


// Uninstall clicked
void MainWindow::on_buttonUninstall_clicked()
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";

    showOutput();

    QString names, postuninstall;

    if (tree == ui->treePopularApps) {
        QTreeWidgetItemIterator it(ui->treePopularApps);
        while (*it) {
            if ((*it)->checkState(1) == Qt::Checked) {
                names += (*it)->text(6).replace("\n", " ") + " ";
                postuninstall += (*it)->text(8) + "\n";
            }
            ++it;
        }
    } else if (tree == ui->treeFlatpak) {
        QString cmd_str = "";
        bool success = true;

        // new version of flatpak takes a "-y" confirmation
        QString conf = "-y ";
        if (fp_ver  < VersionNumber("1.0.1")) {
            conf = "";
        }
        //confirmation dialog
        if (!confirmActions(change_list.join(" "), "remove")) {
            displayFlatpaks(true);
            indexFilterFP.clear();
            listFlatpakRemotes();
            ui->comboRemote->setCurrentIndex(0);
            on_comboRemote_activated(ui->comboRemote->currentIndex());
            ui->comboFilterFlatpak->setCurrentIndex(0);
            QMessageBox::information(this, tr("Done"), tr("Processing finished successfully."));
            ui->tabWidget->blockSignals(true);
            ui->tabWidget->setCurrentWidget(ui->tabFlatpak);
            ui->tabWidget->blockSignals(false);
            enableTabs(true);
            return;
        }

        setCursor(QCursor(Qt::BusyCursor));
        for (const QString &app : change_list) {
            displayOutput();
            if (!cmd.run("runuser -l $(logname) -c \"socat SYSTEM:'flatpak uninstall " + conf + user + app + "',stderr STDIO\"")) { // success if all processed successfuly, failure if one failed
                success = false;
            }
        }
        if (success) { // success if all processed successfuly, failure if one failed
            displayFlatpaks(true);
            indexFilterFP.clear();
            listFlatpakRemotes();
            ui->comboRemote->setCurrentIndex(0);
            on_comboRemote_activated(ui->comboRemote->currentIndex());
            ui->comboFilterFlatpak->setCurrentIndex(0);
            QMessageBox::information(this, tr("Done"), tr("Processing finished successfully."));
            ui->tabWidget->blockSignals(true);
            ui->tabWidget->setCurrentWidget(ui->tabFlatpak);
            ui->tabWidget->blockSignals(false);
        } else {
            QMessageBox::critical(this, tr("Error"), tr("We encountered a problem uninstalling, please check output"));
        }
        enableTabs(true);
        return;
    } else {
        names = change_list.join(" ");
    }

    if (uninstall(names, postuninstall)) {
        if(stable_list.size() > 0) { // update list if it already exists
            buildPackageLists();
        }
        refreshPopularApps();
        QMessageBox::information(this, tr("Success"), tr("Processing finished successfully."));
        ui->tabWidget->setCurrentWidget(tree->parentWidget());
    } else {
        if(stable_list.size() > 0) { // update list if it already exists
            buildPackageLists();
        }
        refreshPopularApps();
        QMessageBox::critical(this, tr("Error"), tr("We encountered a problem uninstalling the program"));
    }
    enableTabs(true);
}

// Actions on switching the tabs
void MainWindow::on_tabWidget_currentChanged(int index)
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";
    ui->tabWidget->setTabText(ui->tabWidget->indexOf(ui->tabOutput), tr("Console Output"));
    ui->buttonInstall->setEnabled(false);
    ui->buttonUninstall->setEnabled(false);

    // reset checkboxes when tab changes
    if (tree != ui->treePopularApps) {
        tree->blockSignals(true);
        tree->clearSelection();
        QTreeWidgetItemIterator it(tree);
        while (*it) {
            (*it)->setCheckState(0, Qt::Unchecked);
            ++it;
        }
        tree->blockSignals(false);
    }

    // save the search text
    QString search_str;
    if (tree == ui->treePopularApps) {
        search_str = ui->searchPopular->text();
    } else if (tree == ui->treeStable) {
        search_str = ui->searchBoxStable->text();
    } else if (tree == ui->treeMXtest) {
        search_str = ui->searchBoxMX->text();
    } else if (tree == ui->treeBackports) {
        search_str = ui->searchBoxBP->text();
    } else if (tree == ui->treeFlatpak) {
        search_str = ui->searchBoxFlatpak->text();
    }

    bool success = false;
    switch (index) {
    case 0:  // Popular
        ui->searchPopular->setText(search_str);
        enableTabs(true);
        setCurrentTree();
        findPopular();
        ui->searchPopular->setFocus();
        break;
    case 1:  // Stable
        ui->searchBoxStable->setText(search_str);
        enableTabs(true);
        setCurrentTree();
        change_list.clear();
        if (tree->topLevelItemCount() == 0) {
            buildPackageLists();
        }
        findPackageOther();
        ui->searchBoxStable->setFocus();
        break;
    case 2:  // Test
        ui->searchBoxMX->setText(search_str);
        enableTabs(true);
        setCurrentTree();
        displayWarning("test");
        change_list.clear();
        if (tree->topLevelItemCount() == 0) {
            buildPackageLists();
        }
        findPackageOther();
        ui->searchBoxMX->setFocus();
        break;
    case 3:  // Backports
        ui->searchBoxBP->setText(search_str);
        enableTabs(true);
        setCurrentTree();
        displayWarning("backports");
        change_list.clear();
        if (tree->topLevelItemCount() == 0) {
            buildPackageLists();
        }
        findPackageOther();
        ui->searchBoxBP->setFocus();
        break;
    case 4: // Flatpak
        ui->searchBoxFlatpak->setText(search_str);
        enableTabs(true);
        setCurrentTree();
        displayWarning("flatpaks");
        blockInterfaceFP(true);

        if(!checkInstalled("flatpak")) {
            int ans = QMessageBox::question(this, tr("Flatpak not installed"), tr("Flatpak is not currently installed.\nOK to go ahead and install it?"));
            if (ans == QMessageBox::No) {
                ui->tabWidget->setCurrentIndex(0);
                break;
            }
            ui->tabWidget->setTabEnabled(ui->tabWidget->indexOf(ui->tabOutput), true);
            ui->tabWidget->setCurrentWidget(ui->tabOutput);
            setCursor(QCursor(Qt::BusyCursor));
            install("flatpak");
            installed_packages = listInstalled();
            if (!checkInstalled("flatpak")) {
                QMessageBox::critical(this, tr("Flatpak not installed"), tr("Flatpak was not installed"));
                ui->tabWidget->setCurrentIndex(0);
                setCursor(QCursor(Qt::ArrowCursor));
                break;
            }
            if (ui->treeStable) { // mark flatpak installed in stable tree
                QHash<QString, VersionNumber> hashInstalled = listInstalledVersions();
                VersionNumber installed = hashInstalled.value("flatpak");
                const QList<QTreeWidgetItem *> found_items  = ui->treeStable->findItems("flatpak", Qt::MatchExactly, 2);
                for (QTreeWidgetItem *item : found_items) {
                    for (int i = 0; i < ui->treeStable->columnCount(); ++i) {
                        item->setForeground(2, QBrush(Qt::gray));
                        item->setForeground(4, QBrush(Qt::gray));
                        item->setToolTip(i, tr("Latest version ") + installed.toString() + tr(" already installed"));
                    }
                    item->setText(5, "installed");
                }
            }
            displayOutput();
            success = cmd.run("runuser -l $(logname) -c \"flatpak remote-add --if-not-exists flathub https://flathub.org/repo/flathub.flatpakrepo\"");
            if (!success) {
                QMessageBox::critical(this, tr("Flathub remote failed"), tr("Flathub remote could not be added"));
                ui->tabWidget->setCurrentIndex(0);
                setCursor(QCursor(Qt::ArrowCursor));
                break;
            }
            listFlatpakRemotes();
            displayFlatpaks(false);
            setCursor(QCursor(Qt::ArrowCursor));
            QMessageBox::warning(this, tr("Needs re-login"), tr("You might need to logout/login to see installed items in the menu"));
            ui->tabWidget->blockSignals(true);
            ui->tabWidget->setCurrentWidget(ui->tabFlatpak);
            ui->tabWidget->blockSignals(false);
            ui->tabWidget->setTabText(ui->tabWidget->indexOf(ui->tabOutput), tr("Console Output"));
            break;
        }
        setCursor(QCursor(Qt::BusyCursor));
        displayOutput();
        success = cmd.run("runuser -l $(logname) -c \"flatpak remote-add --if-not-exists flathub https://flathub.org/repo/flathub.flatpakrepo\"");
        if (!success) {
            QMessageBox::critical(this, tr("Flathub remote failed"), tr("Flathub remote could not be added"));
            ui->tabWidget->setCurrentIndex(0);
            setCursor(QCursor(Qt::ArrowCursor));
            break;
        }
        setCursor(QCursor(Qt::ArrowCursor));
        if (ui->comboRemote->currentText().isEmpty()) {
            listFlatpakRemotes();
        }
        displayFlatpaks(false);
        ui->searchBoxBP->setText(search_str);
        break;
    case 5: // Output
        ui->searchPopular->clear();
        ui->searchBoxStable->clear();
        ui->searchBoxMX->clear();
        ui->searchBoxBP->clear();
        ui->buttonInstall->setDisabled(true);
        ui->buttonUninstall->setDisabled(true);
        break;
    }

    // display Upgrade All button
    ui->buttonUpgradeAll->setVisible((tree == ui->treeStable) && (ui->labelNumUpgr->text().toInt() > 0));
}

// Filter items according to selected filter
void MainWindow::filterChanged(const QString &arg1)
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";
    QList<QTreeWidgetItem *> found_items;
    QTreeWidgetItemIterator it(tree);
    tree->blockSignals(true);

    // filter for Flatpak
    if (tree == ui->treeFlatpak) {
        if (arg1 == tr("Installed runtimes")) {
            displayFiltered(installed_runtimes_fp);
        } else if (arg1 == tr("Installed apps")) {
            displayFiltered(installed_apps_fp);
        } else if (arg1 == tr("All apps")) {
            if(flatpaks_apps.isEmpty()) {
                flatpaks_apps = listFlatpaks(ui->comboRemote->currentText(), "--app");
            }
            displayFiltered(flatpaks_apps, true);
        } else if (arg1 == tr("All runtimes")) {
            if(flatpaks_runtimes.isEmpty()) {
                flatpaks_runtimes = listFlatpaks(ui->comboRemote->currentText(), "--runtime");
            }
            displayFiltered(flatpaks_runtimes, true);
        } else if (arg1 == tr("All available")) {
            int total = 0;
            while (*it) {
                ++total;
                (*it)->setText(6, "true"); // Displayed flag
                (*it)->setHidden(false);
                ++it;
            }
            ui->labelNumAppFP->setText(QString::number(total));
        } else if (arg1 == tr("Not installed")) {
            found_items = tree->findItems("not installed", Qt::MatchExactly, 5);
            ui->labelNumAppFP->setText(QString::number(found_items.count()));
            while (*it) {
                if (found_items.contains(*it)) {
                    (*it)->setHidden(false);
                    (*it)->setText(6, "true"); // Displayed flag
                } else {
                    (*it)->setHidden(true);
                    (*it)->setText(6, "false");
                    (*it)->setCheckState(0, Qt::Unchecked); // uncheck hidden items
                }
                ++it;
            }
        }
        setSearchFocus();
        findPackageOther();
        tree->blockSignals(false);
        return;
    }

    if (arg1 == tr("All packages")) {
        while (*it) {
            (*it)->setText(6, "true"); // Displayed flag
            (*it)->setHidden(false);
            ++it;
        }
        findPackageOther();
        setSearchFocus();
        tree->blockSignals(false);
        return;
    }

    if (arg1 == tr("Upgradable")) {
        found_items = tree->findItems("upgradable", Qt::MatchExactly, 5);
    } else if (arg1 == tr("Installed")) {
        found_items = tree->findItems("installed", Qt::MatchExactly, 5);
    } else if (arg1 == tr("Not installed")) {
        found_items = tree->findItems("not installed", Qt::MatchExactly, 5);
    }

    while (*it) {
        if (found_items.contains(*it)) {
            (*it)->setHidden(false);
            (*it)->setText(6, "true"); // Displayed flag
        } else {
            (*it)->setHidden(true);
            (*it)->setText(6, "false");
            (*it)->setCheckState(0, Qt::Unchecked); // uncheck hidden items
        }
        ++it;
    }
    findPackageOther();
    setSearchFocus();
    tree->blockSignals(false);
}

// When selecting on item in the list
void MainWindow::on_treeStable_itemChanged(QTreeWidgetItem *item)
{
    buildChangeList(item);
}


void MainWindow::on_treeMXtest_itemChanged(QTreeWidgetItem *item)
{
    buildChangeList(item);
}

void MainWindow::on_treeBackports_itemChanged(QTreeWidgetItem *item)
{
    buildChangeList(item);
}

void MainWindow::on_treeFlatpak_itemChanged(QTreeWidgetItem *item)
{
    buildChangeList(item);
}

// Build the change_list when selecting on item in the tree
void MainWindow::buildChangeList(QTreeWidgetItem *item)
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";
    /* if all apps are uninstalled (or some installed) -> enable Install, disable Uinstall
         * if all apps are installed or upgradable -> enable Uninstall, enable Install
         * if all apps are upgradable -> change Install label to Upgrade;
         */

    QString newapp;
    if (tree == ui->treeFlatpak) {
        if (change_list.isEmpty() && indexFilterFP.isEmpty()) { // remember the Flatpak combo location first time this is called
            indexFilterFP = ui->comboFilterFlatpak->currentText();
        }
        newapp = QString(item->text(8)); // full name in column 8
    } else {
        newapp = QString(item->text(2)); // name in column 2
    }

    if (item->checkState(0) == Qt::Checked) {
        ui->buttonInstall->setEnabled(true);
        change_list.append(newapp);
    } else {
        change_list.removeOne(newapp);
    }

    if (tree != ui->treeFlatpak) {
        if (!checkInstalled(change_list)) {
            ui->buttonUninstall->setEnabled(false);
        } else {
            ui->buttonUninstall->setEnabled(true);
        }

        if (checkUpgradable(change_list)) {
            ui->buttonInstall->setText(tr("Upgrade"));
        } else {
            ui->buttonInstall->setText(tr("Install"));
        }
    } else { // for Flatpaks allow selection only of installed or not installed items so one clicks on an installed item only installed items should be displayed and the other way round
        ui->buttonInstall->setText(tr("Install"));
        if (item->text(5) == "installed") {
            if (indexFilterFP == "All apps") { // if "all apps" is selected
                ui->comboFilterFlatpak->setCurrentText(tr("Installed apps"));
            }
            ui->buttonUninstall->setEnabled(true);
            ui->buttonInstall->setEnabled(false);
        } else {
            ui->comboFilterFlatpak->setCurrentText(tr("Not installed"));
            ui->buttonUninstall->setEnabled(false);
            ui->buttonInstall->setEnabled(true);
        }
        if (change_list.isEmpty()) { // reset comboFilterFlatpak if nothing is selected
            ui->comboFilterFlatpak->setCurrentText(indexFilterFP);
            indexFilterFP.clear();
        }
    }

    if (change_list.isEmpty()) {
        ui->buttonInstall->setEnabled(false);
        ui->buttonUninstall->setEnabled(false);
    }
}


// Force repo upgrade
void MainWindow::on_buttonForceUpdateStable_clicked()
{
    ui->searchBoxStable->clear();
    buildPackageLists(true);
}

void MainWindow::on_buttonForceUpdateMX_clicked()
{
    ui->searchBoxMX->clear();
    buildPackageLists(true);
}

void MainWindow::on_buttonForceUpdateBP_clicked()
{
    ui->searchBoxBP->clear();
    buildPackageLists(true);
}

// Hide/unhide lib/-dev packages
void MainWindow::on_checkHideLibs_toggled(bool checked)
{
    ui->checkHideLibsMX->setChecked(checked);
    ui->checkHideLibsBP->setChecked(checked);

    QTreeWidgetItemIterator it(ui->treeStable);
    while (*it) {
        QString app_name = (*it)->text(2);
        if (isFilteredName(app_name) && checked) {
            (*it)->setHidden(true);
        } else {
            (*it)->setHidden(false);
        }
        ++it;
    }
    filterChanged(ui->comboFilterStable->currentText());
}

// Upgrade all packages (from Stable repo only)
void MainWindow::on_buttonUpgradeAll_clicked()
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";
    showOutput();

    QString names;
    QTreeWidgetItemIterator it(ui->treeStable);
    QList<QTreeWidgetItem *> found_items;
    found_items = ui->treeStable->findItems("upgradable", Qt::MatchExactly, 5);

    while (*it) {
        if(found_items.contains(*it)) {
            names += (*it)->text(2) + " ";
        }
        ++it;
    }

    if (install(names)) {
        buildPackageLists();
        QMessageBox::information(this, tr("Done"), tr("Processing finished successfully."));
        ui->tabWidget->setCurrentWidget(tree->parentWidget());
    } else {
        buildPackageLists();
        QMessageBox::critical(this, tr("Error"), tr("Problem detected while installing, please inspect the console output."));
    }

    enableTabs(true);
}


// Pressing Enter or buttonEnter should do the same thing
void MainWindow::on_buttonEnter_clicked()
{
    if (tree == ui->treeFlatpak && ui->lineEdit->text().isEmpty()) { // add "Y" as default response for flatpacks to work like apt-get
        cmd.write("y");
    }
    on_lineEdit_returnPressed();
}

// Send the response to terminal process
void MainWindow::on_lineEdit_returnPressed()
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";
    cmd.write(ui->lineEdit->text().toUtf8() + "\n");
    ui->outputBox->appendPlainText(ui->lineEdit->text() + "\n");
    ui->lineEdit->clear();
    ui->lineEdit->setFocus();
}

void MainWindow::on_buttonCancel_clicked()
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";
    if (cmd.state() != QProcess::NotRunning) {
        if (QMessageBox::warning(this, tr("Quit?"),
                                 tr("Process still running, quitting might leave the system in an unstable state.<p><b>Are you sure you want to exit MX Package Installer?</b>"),
                                 QMessageBox::Yes, QMessageBox::No) == QMessageBox::No) {
            return;
        }
    }
    cleanup();
    qApp->quit();
}


void MainWindow::on_checkHideLibsMX_clicked(bool checked)
{
    ui->checkHideLibs->setChecked(checked);
    ui->checkHideLibsBP->setChecked(checked);
}

void MainWindow::on_checkHideLibsBP_clicked(bool checked)
{
    ui->checkHideLibs->setChecked(checked);
    ui->checkHideLibsMX->setChecked(checked);
}


// on change flatpack remote
void MainWindow::on_comboRemote_activated(int)
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";
    displayFlatpaks(true);
}

void MainWindow::on_buttonUpgradeFP_clicked()
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";
    showOutput();
    setCursor(QCursor(Qt::BusyCursor));
    displayOutput();
    if(cmd.run("runuser -l $(logname) -c \"socat SYSTEM:'flatpak update " + user.trimmed() + "',pty STDIO\"")) {
        displayFlatpaks(true);
        setCursor(QCursor(Qt::ArrowCursor));
        QMessageBox::information(this, tr("Done"), tr("Processing finished successfully."));
        ui->tabWidget->blockSignals(true);
        ui->tabWidget->setCurrentWidget(ui->tabFlatpak);
        ui->tabWidget->blockSignals(false);
    } else {
        setCursor(QCursor(Qt::ArrowCursor));
        QMessageBox::critical(this, tr("Error"), tr("Problem detected while installing, please inspect the console output."));
    }
    enableTabs(true);
}

void MainWindow::on_buttonRemotes_clicked()
{
    ManageRemotes *dialog = new ManageRemotes(this);
    dialog->exec();
    if (dialog->isChanged()) {
        listFlatpakRemotes();
        displayFlatpaks(true);
    }
    if (!dialog->getInstallRef().isEmpty()) {
        showOutput();
        setCursor(QCursor(Qt::BusyCursor));
        displayOutput();
        if (cmd.run("runuser -l $(logname) -c \"socat SYSTEM:'flatpak install -y " + dialog->getUser() + "--from " + dialog->getInstallRef().replace(":", "\\:") + "',stderr STDIO\"")) {
            listFlatpakRemotes();
            displayFlatpaks(true);
            setCursor(QCursor(Qt::ArrowCursor));
            QMessageBox::information(this, tr("Done"), tr("Processing finished successfully."));
            ui->tabWidget->blockSignals(true);
            ui->tabWidget->setCurrentWidget(ui->tabFlatpak);
            ui->tabWidget->blockSignals(false);
        } else {
            setCursor(QCursor(Qt::ArrowCursor));
            QMessageBox::critical(this, tr("Error"), tr("Problem detected while installing, please inspect the console output."));
        }
        enableTabs(true);
    }
}

void MainWindow::on_comboUser_activated(int index)
{
    static bool updated = false;
    if (index == 0) {
        user = "--system ";
    } else {
        user = "--user ";
        if (!updated) {
            setCursor(QCursor(Qt::BusyCursor));
            displayOutput();
            cmd.run("runuser -l $(logname) -c \"flatpak --user remote-add --if-not-exists flathub https://flathub.org/repo/flathub.flatpakrepo\"");
            if (fp_ver >= VersionNumber("1.2.4")) {
                displayOutput();
                cmd.run("runuser -l $(logname) -c \"flatpak update --appstream\"");
            }
            setCursor(QCursor(Qt::ArrowCursor));
            updated = true;
        }
    }
    listFlatpakRemotes();
    displayFlatpaks(true);
}


// process keystrokes
void MainWindow::keyPressEvent(QKeyEvent *event) {
    if (event->key() == Qt::Key_Escape) {
        on_buttonCancel_clicked();
    }
}
