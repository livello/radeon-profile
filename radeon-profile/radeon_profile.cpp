// copyright marazmista 3.2013

// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.

//==================
// the logic is simple. radeon-profile create object based on gpu class.
// gpu class decides what driver is used (device.initialize()). Later, based
// on structure driverFeatures, program activate some ui features.

#include "radeon_profile.h"
#include "ui_radeon_profile.h"

#include <QTimer>
#include <QTextStream>
#include <QMenu>
#include <QDir>
#include <QDateTime>
#include <QMessageBox>
#include <QDebug>

unsigned int radeon_profile::minFanStepsSpeed = 10;


radeon_profile::radeon_profile(QWidget *parent) :
    QMainWindow(parent),
    ui(new Ui::radeon_profile)
{
    ui->setupUi(this);

    // checks if running as root
    if (globalStuff::grabSystemInfo("whoami")[0] == "root") {
         globalStuff::globalConfig.rootMode = true;
        ui->label_rootWarrning->setVisible(true);
    } else {
        globalStuff::globalConfig.rootMode = false;
        ui->label_rootWarrning->setVisible(false);
    }

    if (!device.initialize()) {
        QMessageBox::critical(this,tr("Error"), tr("No Radeon cards have been found in the system."));

        for (int i = 0; i < ui->mainTabs->count() - 1; ++i)
            ui->mainTabs->setTabEnabled(i, false);

        return;
    }

    rangeX = 180;
    ticksCounter = 0;
    statsTickCounter = 0;
	savedState = nullptr;

    // timer init
    timer = new QTimer(this);
    timer->setInterval(ui->spin_timerInterval->value()*1000);

    setupUiElements();

    for (int i = 0; i < device.gpuList.count(); ++i)
        ui->combo_gpus->addItem(device.gpuList.at(i).sysName);

    loadConfig();

    // start is heavy, delegated to another thread to show ui smoothly
    connect(&initFuture, SIGNAL(finished()), this,  SLOT(initFutureHandler()));
    initFuture.setFuture(QtConcurrent::run(this, &radeon_profile::refreshGpuData));

    if (ui->cb_enableOverclock->isChecked() && ui->cb_overclockAtLaunch->isChecked())
        ui->btn_applyOverclock->click();

    // fill tables with data at the start //
    ui->list_glxinfo->addItems(device.getGLXInfo(ui->combo_gpus->currentText()));
    fillConnectors();
    fillModInfo();

    timer->start();
    timerEvent();

    showWindow();
}

radeon_profile::~radeon_profile()
{
    delete ui;
}

void radeon_profile::initFutureHandler() {
    setupUiEnabledFeatures(device.getDriverFeatures(), device.gpuData);
    ui->centralWidget->setEnabled(true);

    connectSignals();
    refreshUI();
}

void radeon_profile::connectSignals()
{
    // fix for warrning: QMetaObject::connectSlotsByName: No matching signal for...
    connect(ui->combo_gpus,SIGNAL(currentIndexChanged(QString)),this,SLOT(gpuChanged()));
    connect(ui->combo_pProfile,SIGNAL(currentIndexChanged(int)),this,SLOT(changeProfileFromCombo()));
    connect(ui->combo_pLevel,SIGNAL(currentIndexChanged(int)),this,SLOT(changePowerLevelFromCombo()));

    connect(timer,SIGNAL(timeout()),this,SLOT(timerEvent()));
    connect(ui->timeSlider,SIGNAL(valueChanged(int)),this,SLOT(changeTimeRange()));
}

void radeon_profile::setupUiElements()
{
    ui->mainTabs->setCurrentIndex(0);
    ui->tabs_systemInfo->setCurrentIndex(0);
    ui->configGroups->setCurrentIndex(0);
    ui->list_currentGPUData->setHeaderHidden(false);
    ui->execPages->setCurrentIndex(0);
    setupGraphs();
    setupForcePowerLevelMenu();
    setupOptionsMenu();
    setupContextMenus();
    setupTrayIcon();
    addRuntimeWidgets();
    ui->centralWidget->setEnabled(false);
}


void radeon_profile::addRuntimeWidgets() {
    // add button for manual refresh glx info, connectors, mod params
    QPushButton *refreshBtn = new QPushButton();
    refreshBtn->setIcon(QIcon(":/icon/symbols/refresh.png"));
    ui->tabs_systemInfo->setCornerWidget(refreshBtn);
    refreshBtn->setIconSize(QSize(20,20));
    refreshBtn->show();
    connect(refreshBtn,SIGNAL(clicked()),this,SLOT(refreshBtnClicked()));

    ui->label_version->setText(tr("version %n", NULL, appVersion));

    // version label
    QLabel *l = new QLabel("v. " +QString().setNum(appVersion),this);
    QFont f;
    f.setStretch(QFont::Unstretched);
    f.setWeight(QFont::Bold);
    f.setPointSize(8);
    l->setFont(f);
    ui->mainTabs->setCornerWidget(l,Qt::BottomRightCorner);
    l->show();

    // button on exec pages
    QPushButton *btnBackProfiles = new QPushButton();
    btnBackProfiles->setText(tr("Back to profiles"));
    ui->tabs_execOutputs->setCornerWidget(btnBackProfiles);
    btnBackProfiles->show();
    connect(btnBackProfiles,SIGNAL(clicked()),this,SLOT(btnBackToProfilesClicked()));

    // set pwm buttons in group
    pwmGroup.addButton(ui->btn_pwmAuto);
    pwmGroup.addButton(ui->btn_pwmFixed);
    pwmGroup.addButton(ui->btn_pwmProfile);
}

// based on driverFeatures structure returned by gpu class, adjust ui elements
void radeon_profile::setupUiEnabledFeatures(const DriverFeatures &features, const QMap<ValueID, RPValue> &data) {
    if (features.canChangeProfile && features.currentPowerMethod < PowerMethod::PM_UNKNOWN) {
        ui->tabs_pm->setTabEnabled(0,features.currentPowerMethod == PowerMethod::PROFILE);

        ui->tabs_pm->setTabEnabled(1,features.currentPowerMethod == PowerMethod::DPM);
        changeProfile->setEnabled(features.currentPowerMethod == PowerMethod::PROFILE);
        dpmMenu->setEnabled(features.currentPowerMethod == PowerMethod::DPM);
        ui->combo_pLevel->setEnabled(features.currentPowerMethod == PowerMethod::DPM);
    } else {
        ui->tabs_pm->setEnabled(false);
        changeProfile->setEnabled(false);
        dpmMenu->setEnabled(false);
        ui->combo_pLevel->setEnabled(false);
        ui->combo_pProfile->setEnabled(false);
        ui->cb_eventsTracking->setEnabled(false);
        ui->cb_eventsTracking->setChecked(false);
    }

    ui->cb_showFreqGraph->setEnabled(data.contains(ValueID::CLK_CORE));
    ui->tabs_systemInfo->setTabEnabled(3,data.contains(ValueID::CLK_CORE));
    ui->cb_showVoltsGraph->setEnabled(data.contains(ValueID::VOLT_CORE));

    if (!device.gpuData.contains(ValueID::TEMPERATURE_CURRENT)) {
        ui->cb_showTempsGraph->setEnabled(false);
        ui->plotTemp->setVisible(false);
        ui->l_temp->setVisible(false);
    }

    if (!device.gpuData.contains(ValueID::CLK_CORE) && !data.contains(ValueID::TEMPERATURE_CURRENT) && !device.gpuData.contains(ValueID::VOLT_CORE))
        ui->mainTabs->setTabEnabled(1,false);

    if (!device.gpuData.contains(ValueID::CLK_CORE) && !data.contains(ValueID::CLK_MEM)) {
        ui->l_cClk->setVisible(false);
        ui->l_mClk->setVisible(false);
    }

    if (!device.gpuData.contains(ValueID::VOLT_CORE) && !data.contains(ValueID::VOLT_MEM)) {
        ui->l_cVolt->setVisible(false);
        ui->l_mVolt->setVisible(false);
    }

    if (data.contains(ValueID::FAN_SPEED_PERCENT) && features.canChangeProfile) {
        qDebug() << "Fan control is available , configuring the fan control tab";
        on_fanSpeedSlider_valueChanged(ui->fanSpeedSlider->value());
        ui->l_fanProfileUnsavedIndicator->setVisible(false);

        setupFanProfilesMenu();

        if (ui->cb_saveFanMode->isChecked()) {
            switch (ui->fanModesTabs->currentIndex()) {
                case 1:
                    ui->btn_pwmFixed->click();
                    break;
                case 2:
                    device.getTemperature();
                    ui->btn_pwmProfile->click();
                    break;
            }
        }
    } else {
        qDebug() << "Fan control is not available";
        ui->mainTabs->setTabEnabled(3,false);
        ui->l_fanSpeed->setVisible(false);
    }

    if (Q_LIKELY(features.currentPowerMethod == PowerMethod::DPM)) {
        ui->combo_pProfile->addItems(globalStuff::createDPMCombo());
        ui->combo_pLevel->addItems(globalStuff::createPowerLevelCombo());
    } else {
        ui->combo_pLevel->setEnabled(false);
        ui->combo_pProfile->addItems(globalStuff::createProfileCombo());
    }

    ui->configGroups->setTabEnabled(2,device.daemonConnected() && device.getDriverFeatures().clocksSource == ClocksDataSource::PM_FILE);
    ui->mainTabs->setTabEnabled(2,features.ocCoreAvailable);
}

void radeon_profile::refreshGpuData() {
    device.refreshPowerLevel();
    device.getClocks();
    device.getTemperature();
    device.getGpuLoad();
    device.getPwmSpeed();

    if (Q_LIKELY(execsRunning.count() == 0))
        return;

    updateExecLogs();
}

void radeon_profile::addChild(QTreeWidget * parent, const QString &leftColumn, const QString  &rightColumn) {
    parent->addTopLevelItem(new QTreeWidgetItem(QStringList() << leftColumn << rightColumn));
}

// -1 value means that we not show in table. it's default (in gpuClocksStruct constructor), and if we
// did not alter it, it stays and in result will be not displayed
void radeon_profile::refreshUI() {
    ui->l_cClk->setText(device.gpuData.value(ValueID::CLK_CORE).strValue);
    ui->l_mClk->setText(device.gpuData.value(ValueID::CLK_MEM).strValue);
    ui->l_mVolt->setText(device.gpuData.value(ValueID::VOLT_CORE).strValue);
    ui->l_cVolt->setText(device.gpuData.value(ValueID::VOLT_MEM).strValue);

    // Header - Temperature
    ui->l_temp->setText(device.gpuData.value(ValueID::TEMPERATURE_CURRENT).strValue);

    // Header - Fan speed
    ui->l_fanSpeed->setText(device.gpuData.value(ValueID::FAN_SPEED_PERCENT).strValue);

    ui->l_gpuUsage->setText(device.gpuData.value(ValueID::GPU_LOAD_PERCENT).strValue);

    // GPU data list
    if (ui->mainTabs->currentIndex() == 0) {
        ui->list_currentGPUData->clear();

        if (device.gpuData.contains(ValueID::POWER_LEVEL))
            addChild(ui->list_currentGPUData, tr("Power level"), device.gpuData.value(ValueID::POWER_LEVEL).strValue);
        if (device.gpuData.contains(ValueID::CLK_CORE))
            addChild(ui->list_currentGPUData, tr("GPU clock"), device.gpuData.value(ValueID::CLK_CORE).strValue);
        if (device.gpuData.contains(ValueID::CLK_MEM))
            addChild(ui->list_currentGPUData, tr("Memory clock"), device.gpuData.value(ValueID::CLK_MEM).strValue);
        if (device.gpuData.contains(ValueID::CLK_UVD))
            addChild(ui->list_currentGPUData, tr("UVD core clock (cclk)"), device.gpuData.value(ValueID::CLK_UVD).strValue);
        if (device.gpuData.contains(ValueID::DCLK_UVD))
            addChild(ui->list_currentGPUData, tr("UVD decoder clock (dclk)"), device.gpuData.value(ValueID::DCLK_UVD).strValue);
        if (device.gpuData.contains(ValueID::VOLT_CORE))
            addChild(ui->list_currentGPUData, tr("GPU voltage (vddc)"), device.gpuData.value(ValueID::VOLT_CORE).strValue);
        if (device.gpuData.contains(ValueID::VOLT_MEM))
            addChild(ui->list_currentGPUData, tr("I/O voltage (vddci)"), device.gpuData.value(ValueID::VOLT_MEM).strValue);

        if (ui->list_currentGPUData->topLevelItemCount() == 0)
            addChild(ui->list_currentGPUData, tr("Can't read data"), tr("You need debugfs mounted and either root rights or the daemon running"));

        addChild(ui->list_currentGPUData, tr("GPU temperature"), device.gpuData.value(ValueID::TEMPERATURE_CURRENT).strValue);
    }
}

void radeon_profile::updateExecLogs() {
    for (int i = 0; i < execsRunning.count(); i++) {
        if (execsRunning.at(i)->getExecState() == QProcess::Running && execsRunning.at(i)->logEnabled) {
            QString logData = QDateTime::currentDateTime().toString(logDateFormat) +";" + device.gpuData.value(ValueID::POWER_LEVEL, RPValue()).strValue + ";" +
                    device.gpuData.value(ValueID::CLK_CORE).strValue + ";"+
                    device.gpuData.value(ValueID::CLK_MEM).strValue + ";"+
                    device.gpuData.value(ValueID::CLK_UVD).strValue + ";"+
                    device.gpuData.value(ValueID::DCLK_UVD).strValue + ";"+
                    device.gpuData.value(ValueID::VOLT_CORE).strValue + ";"+
                    device.gpuData.value(ValueID::VOLT_MEM).strValue + ";"+
                    device.gpuData.value(ValueID::TEMPERATURE_CURRENT).strValue;
            execsRunning.at(i)->appendToLog(logData);
        }
    }
}
//===================================
// === Main timer loop  === //
void radeon_profile::timerEvent() {
    if (!refreshWhenHidden->isChecked() && this->isHidden()) {

        // even if in tray, keep the fan control active (if enabled)
        if (device.gpuData.contains(ValueID::FAN_SPEED_PERCENT) && ui->btn_pwmProfile->isChecked()) {
            device.getTemperature();
            adjustFanSpeed();
        }
        return;
    }

    if (Q_LIKELY(ui->cb_gpuData->isChecked())) {
        refreshGpuData();

        ui->combo_pProfile->setCurrentIndex(ui->combo_pProfile->findText(device.currentPowerProfile));
        if (device.getDriverFeatures().currentPowerMethod == PowerMethod::DPM)
            ui->combo_pLevel->setCurrentIndex(ui->combo_pLevel->findText(device.currentPowerLevel));

        if (device.gpuData.contains(ValueID::FAN_SPEED_PERCENT) && ui->btn_pwmProfile->isChecked())
            adjustFanSpeed();


        // lets say coreClk is essential to get stats (it is disabled in ui anyway when features.clocksAvailable is false)
        if (ui->cb_stats->isChecked() && device.gpuData.contains(ValueID::CLK_CORE)) {
            doTheStats();

            // do the math only when user looking at stats table
            if (ui->tabs_systemInfo->currentIndex() == 3 && ui->mainTabs->currentIndex() == 0)
                updateStatsTable();
        }
        refreshUI();
    }

    if (Q_LIKELY(ui->cb_graphs->isChecked()))
        refreshGraphs();

    if (ui->cb_glxInfo->isChecked()) {
        ui->list_glxinfo->clear();
        ui->list_glxinfo->addItems(device.getGLXInfo(ui->combo_gpus->currentText()));
    }

    if (ui->cb_connectors->isChecked())
        fillConnectors();

    if (ui->cb_modParams->isChecked())
        fillModInfo();

    refreshTooltip();

    if (ui->cb_eventsTracking->isChecked())
        checkEvents();
}

/**
 * If the fan profile contains a direct match for the current temperature, it is used.<br>
 * Otherwise find the closest matches and use linear interpolation:<br>
 * \f$speed(temp) = \frac{hSpeed - lSpeed}{hTemperature - lTemperature} * (temp - lTemperature)  + lSpeed\f$<br>
 * Where:<br>
 * \f$(hTemperature, hSpeed)\f$ is the higher closest match;<br>
 * \f$(lTemperature, lSpeed)\f$ is the lower closest match;<br>
 * \f$temp\f$ is the current temperature;<br>
 * \f$speed\f$ is the speed to apply.
 */
void radeon_profile::adjustFanSpeed() {
    if (device.gpuData.value(ValueID::TEMPERATURE_CURRENT).value != device.gpuData.value(ValueID::TEMPERATURE_BEFORE_CURRENT).value ) {
        if (currentFanProfile.contains(device.gpuData.value(ValueID::TEMPERATURE_CURRENT).value)) {  // Exact match
            device.setPwmValue(currentFanProfile.value(device.gpuData.value(ValueID::TEMPERATURE_CURRENT).value));
            return;
        }

        // find bounds of current temperature
        QMap<int,unsigned int>::const_iterator high = currentFanProfile.upperBound(device.gpuData.value(ValueID::TEMPERATURE_CURRENT).value);
        QMap<int,unsigned int>::const_iterator low = (currentFanProfile.size() > 1 ? high - 1 : high);

        int hSpeed = high.value(),
			lSpeed = low.value();

        if (high == currentFanProfile.constBegin()) {
            device.setPwmValue(hSpeed);
            return;
        }

        if (low == currentFanProfile.constEnd()) {
            device.setPwmValue(lSpeed);
            return;
        }

        // calculate two point stright line equation based on boundaries of current temperature
        // y = mx + b = (y2-y1)/(x2-x1)*(x-x1)+y1
        int hTemperature = high.key(),
                lTemperature = low.key();

        float speed = (float)(hSpeed - lSpeed) / (float)(hTemperature - lTemperature)  * (device.gpuData.value(ValueID::TEMPERATURE_CURRENT).value - lTemperature)  + lSpeed;

        device.setPwmValue((speed < minFanStepsSpeed) ? minFanStepsSpeed : speed);
    }
}


void radeon_profile::refreshGraphs() {
    // count the tick and move graph to right
    ticksCounter++;
    ui->plotTemp->xAxis->setRange(ticksCounter + globalStuff::globalConfig.graphOffset, rangeX,Qt::AlignRight);
    ui->plotTemp->replot();

    ui->plotClocks->xAxis->setRange(ticksCounter + globalStuff::globalConfig.graphOffset,rangeX,Qt::AlignRight);
    ui->plotClocks->replot();

    ui->plotVolts->xAxis->setRange(ticksCounter + globalStuff::globalConfig.graphOffset,rangeX,Qt::AlignRight);
    ui->plotVolts->replot();

    // choose bigger clock and adjust plot scale
    int r = (device.gpuData.value(ValueID::CLK_MEM).value >= device.gpuData.value(ValueID::CLK_CORE).value) ? device.gpuData.value(ValueID::CLK_MEM).value : device.gpuData.value(ValueID::CLK_CORE).value;
    if (r > ui->plotClocks->yAxis->range().upper)
        ui->plotClocks->yAxis->setRangeUpper(r + 150);

    // add data to plots
    ui->plotClocks->graph(0)->addData(ticksCounter, device.gpuData.value(ValueID::CLK_CORE).value);
    ui->plotClocks->graph(1)->addData(ticksCounter, device.gpuData.value(ValueID::CLK_MEM).value);
    ui->plotClocks->graph(2)->addData(ticksCounter, device.gpuData.value(ValueID::CLK_UVD).value);
    ui->plotClocks->graph(3)->addData(ticksCounter, device.gpuData.value(ValueID::DCLK_UVD).value);

    if (device.gpuData.value(ValueID::VOLT_CORE).value > ui->plotVolts->yAxis->range().upper)
        ui->plotVolts->yAxis->setRangeUpper(device.gpuData.value(ValueID::VOLT_CORE).value + 100);

    ui->plotVolts->graph(0)->addData(ticksCounter,device.gpuData.value(ValueID::VOLT_CORE).value);
    ui->plotVolts->graph(1)->addData(ticksCounter,device.gpuData.value(ValueID::VOLT_MEM).value);

    // temperature graph
    if (device.gpuData.value(ValueID::TEMPERATURE_MAX).value >= ui->plotTemp->yAxis->range().upper || device.gpuData.value(ValueID::TEMPERATURE_MIN).value <= ui->plotTemp->yAxis->range().lower)
        ui->plotTemp->yAxis->setRange(device.gpuData.value(ValueID::TEMPERATURE_MIN).value - 5, device.gpuData.value(ValueID::TEMPERATURE_MAX).value + 5);

    ui->plotTemp->graph(0)->addData(ticksCounter, device.gpuData.value(ValueID::TEMPERATURE_CURRENT).value);
    ui->l_minMaxTemp->setText("Max: " + device.gpuData.value(ValueID::TEMPERATURE_MAX).strValue  + " | Min: " + device.gpuData.value(ValueID::TEMPERATURE_MIN).strValue);
}

void radeon_profile::doTheStats() {
    // count ticks for stats //
    statsTickCounter++;

    // figure out pm level based on data provided
    QString pmLevelName = "Core: " + device.gpuData.value(ValueID::CLK_CORE).strValue + "  Mem: " + device.gpuData.value(ValueID::CLK_MEM).strValue;

    if (pmStats.contains(pmLevelName)) // This power level already exists, increment its count
        pmStats[pmLevelName]++;
    else { // This power level does not exist, create it
        pmStats.insert(pmLevelName, 1);
        ui->list_stats->addTopLevelItem(new QTreeWidgetItem(QStringList() << pmLevelName));
        ui->list_stats->header()->resizeSections(QHeaderView::ResizeToContents);
    }
}

void radeon_profile::updateStatsTable() {
    // do the math with percents
    for(QTreeWidgetItemIterator item(ui->list_stats); *item; item++) // For each item in list_stats
        (*item)->setText(1,QString::number(pmStats.value((*item)->text(0))*100.0/statsTickCounter)+"%");
}

void radeon_profile::refreshTooltip()
{
    QString tooltipdata = radeon_profile::windowTitle() + "\n" + tr("Current profile: ")+ device.currentPowerProfile + "  " + device.currentPowerLevel +"\n";

    for (short i = 0; i < ui->list_currentGPUData->topLevelItemCount(); i++)
        tooltipdata += ui->list_currentGPUData->topLevelItem(i)->text(0) + ": " + ui->list_currentGPUData->topLevelItem(i)->text(1) + '\n';

    tooltipdata.remove(tooltipdata.length() - 1, 1); //remove empty line at bootom
    trayIcon->setToolTip(tooltipdata);
}

void radeon_profile::configureDaemonAutoRefresh (bool enabled, int interval) {
    globalStuff::globalConfig.daemonAutoRefresh = enabled;

    if(enabled)
        globalStuff::globalConfig.interval = interval;

    device.reconfigureDaemon();
}

bool radeon_profile::askConfirmation(const QString title, const QString question){
    return QMessageBox::Yes ==
            QMessageBox::question(this, title, question, QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
}

void radeon_profile::showWindow() {
    if (ui->cb_minimizeTray->isChecked() && ui->cb_startMinimized->isChecked())
        return;

    if (ui->cb_startMinimized->isChecked())
        this->showMinimized();
    else
        this->showNormal();
}
