/* * This file is part of m-keyboard *
 *
 * Copyright (C) 2010 Nokia Corporation and/or its subsidiary(-ies).
 * All rights reserved.
 * Contact: Nokia Corporation (directui@nokia.com)
 *
 * If you have questions regarding the use of this file, please contact
 * Nokia at directui@nokia.com.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1 as published by the Free Software Foundation
 * and appearing in the file LICENSE.LGPL included in the packaging
 * of this file.
 */

#include "flickgesturerecognizer.h"
#include "mkeyboardhost.h"
#include "mkeyboardhost_p.h"
#include "mvirtualkeyboardstyle.h"
#include "mvirtualkeyboard.h"
#include "mhardwarekeyboard.h"
#include "mimcorrectionhost.h"
#include "keyboarddata.h"
#include "layoutsmanager.h"
#include "symbolview.h"
#include "mimtoolbar.h"
#include "sharedhandlearea.h"

#include <mimenginewordsinterfacefactory.h>
#include <mabstractinputmethodhost.h>
#include <mplainwindow.h>
#include <mtoolbardata.h>
#include <mgconfitem.h>
#include <mtheme.h>

#include <QDebug>
#include <QCoreApplication>
#include <QKeyEvent>
#include <QFile>
#include <QRegExp>

#include <MApplication>
#include <MComponentData>
#include <MFeedbackPlayer>
#include <mreactionmap.h>
#include <MScene>
#include <MSceneManager>
#include <MSceneWindow>
#include <MBanner>
#include <MLocale>
#include <MBreakIterator>
#include <MLibrary>

M_LIBRARY

namespace
{
    const QString InputMethodList("MInputMethodList");
    const QString DefaultInputLanguage("en_GB");
    // TODO: check that these paths still hold
    const QString InputMethodCorrectionSetting("/meegotouch/inputmethods/virtualkeyboard/correctionenabled");
    bool DefaultInputMethodCorrectionSettingOption = true;
    const QString InputMethodCorrectionEngine("/meegotouch/inputmethods/correctionengine");
    const QString AutoCapsSentenceDelimiters(".?!¡¿"); // used as regexp character set content!
    const int MaximumErrorCorrectionCandidate = 5;
    const int RotationDuration = 750; //! After vkb hidden, how long to wait until shown again
    const int AutoBackspaceDelay = 500;      // in ms
    const int LongPressTime = 600;           // in ms
    const int BackspaceRepeatInterval = 100; // in ms
    const int MultitapTime = 1500;           // in ms
    const Qt::KeyboardModifier FnLevelModifier = Qt::GroupSwitchModifier;
    // This GConf item defines whether multitouch is enabled or disabled
    const char * const MultitouchSettings = "/meegotouch/inputmethods/multitouch/enabled";
}

MKeyboardHost::CycleKeyHandler::CycleKeyHandler(MKeyboardHost &parent)
    : QObject(&parent),
    host(parent),
    timer(),
    cycleText(),
    prevEvent()
{
    timer.setSingleShot(true);
    timer.setInterval(MultitapTime);
    connect(&timer, SIGNAL(timeout()), this, SLOT(commitCycleKey()));
}

MKeyboardHost::CycleKeyHandler::~CycleKeyHandler()
{
}

bool MKeyboardHost::CycleKeyHandler::handleTextInputKeyClick(const KeyEvent &event)
{
    // If there's a pending keypress, handle it:
    if (timer.isActive() && !(event == prevEvent)) {
        timer.stop();
        commitCycleKey();
    }

    if (event.specialKey() != KeyEvent::CycleSet) {
        return false;  // Host should handle non-cyclekeys.
    }

    if (event.text().length() == 0) {
        qWarning() << __PRETTY_FUNCTION__ << "Empty cycleset in layout";
        return true;
    }

    if (event == prevEvent) {
        // Cycling through same cycle key
        if (host.preedit.length() > 0) {
            // FIXME: appending the cycle key to the preedit --> pressing cycle key
            // when there already is something in preedit will cause the whole preedit
            // to be commited after timeout
            host.setPreedit(host.preedit.left(host.preedit.length() - 1));
        }
        cycleIndex = (cycleIndex + 1) % cycleText.size();
    }
    else {
        // Different key than last time, reset index and cycle text
        prevEvent = event;
        cycleIndex = 0;
        cycleText = event.text();
    }

    host.preedit += cycleText[cycleIndex];
    host.inputMethodHost()->sendPreeditString(host.preedit,
                                              MInputMethod::PreeditNoCandidates);

    timer.start();

    return true;
}

void MKeyboardHost::CycleKeyHandler::commitCycleKey()
{
    if (cycleText.length() > 0) {
        host.sendString(host.preedit);
        host.preedit.clear();
        cycleText.clear();
        prevEvent = KeyEvent();
    }
}

MKeyboardHost::MKeyboardHost(MAbstractInputMethodHost *imHost, QObject *parent)
    : MAbstractInputMethod(imHost, parent),
      vkbStyleContainer(0),
      correctionHost(0),
      vkbWidget(0),
      symbolView(0),
      imCorrectionEngine(0),
      inputMethodCorrectionSettings(new MGConfItem(InputMethodCorrectionSetting)),
      inputMethodCorrectionEngine(new MGConfItem(InputMethodCorrectionEngine)),
      angle(M::Angle0),
      rotationInProgress(false),
      correctionEnabled(false),
      feedbackPlayer(0),
      autoCapsEnabled(true),
      autoCapsTriggered(false),
      cursorPos(-1),
      inputMethodMode(M::InputMethodModeNormal),
      backspaceTimer(),
      shiftHeldDown(false),
      activeState(MInputMethod::OnScreen),
      modifierLockOnBanner(0),
      haveFocus(false),
      enableMultiTouch(false),
      cycleKeyHandler(new CycleKeyHandler(*this)),
      currentIndicatorDeadKey(false),
      engineLayoutDirty(false),
      backspaceMode(NormalBackspace)
{
    displayHeight = MPlainWindow::instance()->visibleSceneSize(M::Landscape).height();
    displayWidth  = MPlainWindow::instance()->visibleSceneSize(M::Landscape).width();

    enableMultiTouch = MGConfItem(MultitouchSettings).value().toBool();

    sceneWindow = new MSceneWindow;
    sceneWindow->setManagedManually(true); // we want the scene window to remain in origin

    // This will add scene window as child of MSceneManager's root element
    // which is the QGraphicsItem that is rotated when orientation changes.
    // It uses animation to carry out the orientation change transform
    // (e.g. rotation and position animation). We do this because transform
    // happens in the scene, not in the view (MWindow) anymore.
    // Enforcing full viewport updates helps to paint correctly in software mode.
    MPlainWindow::instance()->sceneManager()->appearSceneWindowNow(sceneWindow);
    MPlainWindow::instance()->setViewportUpdateMode(QGraphicsView::FullViewportUpdate);

    // Because we set vkbWidget as a child of sceneWindow the vkb
    // will always be in correct orientation. However the animation will be
    // affected as well. If we want to keep the current hiding/showing animation
    // (up & down) without getting it combined with the rotation animation
    // we have at least two options:
    // 1) Make our own MOrientationAnimation when libmeegotouch begins supporting
    //    setting it, through theme probably.
    // 2) Add widgets directly to scene (detached from MSceneManager) and
    //    update their transformations by hand.

    vkbStyleContainer = new MVirtualKeyboardStyleContainer;
    vkbStyleContainer->initialize("MVirtualKeyboard", "MVirtualKeyboardView", 0);

    LayoutsManager::createInstance();

    FlickGestureRecognizer::registerSharedRecognizer();

    vkbWidget = new MVirtualKeyboard(LayoutsManager::instance(), vkbStyleContainer, sceneWindow);
    vkbWidget->setInputMethodMode(static_cast<M::InputMethodMode>(inputMethodMode));

    connect(vkbWidget, SIGNAL(keyClicked(const KeyEvent &)),
            this, SLOT(handleKeyClick(const KeyEvent &)));
    connect(vkbWidget, SIGNAL(keyPressed(const KeyEvent &)),
            this, SLOT(handleKeyPress(const KeyEvent &)));
    connect(vkbWidget, SIGNAL(keyReleased(const KeyEvent &)),
            this, SLOT(handleKeyRelease(const KeyEvent &)));
    connect(vkbWidget, SIGNAL(longKeyPressed(const KeyEvent &)),
            this, SLOT(handleLongKeyPress(const KeyEvent &)));
    connect(vkbWidget, SIGNAL(showSymbolViewRequested()),
            this, SLOT(showSymbolView()));

    connect(vkbWidget, SIGNAL(regionUpdated(const QRegion &)),
            this, SLOT(handleRegionUpdate(const QRegion &)));
    connect(vkbWidget, SIGNAL(inputMethodAreaUpdated(const QRegion &)),
            this, SLOT(handleInputMethodAreaUpdate(const QRegion &)));

    connect(vkbWidget, SIGNAL(userInitiatedHide()),
            this, SLOT(userHide()));

    connect(vkbWidget, SIGNAL(pluginSwitchRequired(MInputMethod::SwitchDirection)),
            this, SLOT(switchPlugin(MInputMethod::SwitchDirection)));

    // construct hardware keyboard object
    hardwareKeyboard = new MHardwareKeyboard(*imHost, this);
    connect(hardwareKeyboard, SIGNAL(symbolKeyClicked()),
            this, SLOT(handleSymbolKeyClick()));

    bool ok = connect(vkbWidget, SIGNAL(copyPasteClicked(CopyPasteState)),
                      this, SLOT(sendCopyPaste(CopyPasteState)));
    Q_UNUSED(ok); // if Q_NO_DEBUG is defined then the assert won't be used
    Q_ASSERT(ok);

    imToolbar = new MImToolbar;

    ok = connect(imToolbar, SIGNAL(copyPasteRequest(CopyPasteState)),
                 this, SLOT(sendCopyPaste(CopyPasteState)));
    Q_ASSERT(ok);
    ok = connect(imToolbar, SIGNAL(sendKeyEventRequest(const QKeyEvent &)),
                 this, SLOT(sendKeyEvent(const QKeyEvent &)));
    Q_ASSERT(ok);
    ok = connect(imToolbar, SIGNAL(sendStringRequest(const QString &)),
                 this, SLOT(sendStringFromToolbar(const QString &)));
    Q_ASSERT(ok);
    ok = connect(imToolbar, SIGNAL(copyPasteClicked(CopyPasteState)),
                 this, SLOT(sendCopyPaste(CopyPasteState)));
    Q_ASSERT(ok);
    ok = connect(imToolbar, SIGNAL(closeKeyboardRequest()),
                 this, SLOT(userHide()));
    Q_ASSERT(ok);

    sharedHandleArea = new SharedHandleArea(*imToolbar, sceneWindow);
    sharedHandleArea->resize(MPlainWindow::instance()->visibleSceneSize().width(),
                             sharedHandleArea->size().height());
    ok = connect(imToolbar, SIGNAL(regionUpdated()),
                 sharedHandleArea, SLOT(updatePositionAndRegion()));
    Q_ASSERT(ok);
    sharedHandleArea->setInputMethodMode(static_cast<M::InputMethodMode>(inputMethodMode));

    ok = connect(sharedHandleArea, SIGNAL(regionUpdated()),
                 this, SLOT(handleRegionUpdate()));
    Q_ASSERT(ok);

    ok = connect(sharedHandleArea, SIGNAL(inputMethodAreaUpdated()),
                 this, SLOT(handleInputMethodAreaUpdate()));
    Q_ASSERT(ok);

    // Set z value below default level (0.0) so popup will be on top of shared handle area.
    sharedHandleArea->setZValue(-1.0);

    vkbWidget->setSharedHandleArea(sharedHandleArea);
    sharedHandleArea->watchOnWidget(vkbWidget);


    createCorrectionCandidateWidget();

    // Don't listen to device orientation.  Applications can be in different orientation
    // than the device (especially plain qt apps). See NB#185013 - Locking VKB orientation.
    MPlainWindow::instance()->lockOrientationAngle();

    // Ideally we would adjust the hiding/showing animation of vkb according to
    // animation of the application receiving input. For example, with 3-phase
    // MBasicOrientationAnimation we would probably want to sync like this:
    // 1) Navigation bar hiding (altough it's probably already hidden) -> vkb hiding
    // 2) Rotation in progress -> vkb not visible
    // 3) Navigation bar showing -> vkb showing
    connect(MPlainWindow::instance()->sceneManager(),
            SIGNAL(orientationAngleChanged(M::OrientationAngle)),
            SLOT(prepareOrientationChange()));

    // orientationChangeFinished is emitted on every angle change,
    // not only orientation change.
    connect(MPlainWindow::instance()->sceneManager(),
            SIGNAL(orientationChangeFinished(M::Orientation)),
            SLOT(finalizeOrientationChange()));

    symbolView = new SymbolView(LayoutsManager::instance(), vkbStyleContainer,
                                vkbWidget->selectedLayout(), sceneWindow);
    connect(symbolView, SIGNAL(regionUpdated(const QRegion &)),
            this, SLOT(handleRegionUpdate(const QRegion &)));
    connect(symbolView, SIGNAL(regionUpdated(const QRegion &)),
            this, SLOT(handleInputMethodAreaUpdate(const QRegion &)));

    connect(symbolView, SIGNAL(updateReactionMap()),
            this, SLOT(updateReactionMaps()));

    connect(symbolView, SIGNAL(keyClicked(const KeyEvent &)),
            this, SLOT(handleKeyClick(const KeyEvent &)));
    connect(symbolView, SIGNAL(keyPressed(const KeyEvent &)),
            this, SLOT(handleKeyPress(const KeyEvent &)));
    connect(symbolView, SIGNAL(keyReleased(const KeyEvent &)),
            this, SLOT(handleKeyRelease(const KeyEvent &)));
    connect(symbolView, SIGNAL(longKeyPressed(const KeyEvent &)),
            this, SLOT(handleLongKeyPress(const KeyEvent &)));

    symbolView->setSharedHandleArea(sharedHandleArea);
    sharedHandleArea->watchOnWidget(symbolView);

    connect(vkbWidget, SIGNAL(layoutChanged(const QString &)),
            this, SLOT(handleVirtualKeyboardLayoutChanged(const QString &)));

    connect(vkbWidget, SIGNAL(shiftLevelChanged()),
            this, SLOT(updateSymbolViewLevel()));

    connect(hardwareKeyboard, SIGNAL(shiftStateChanged()),
            this, SLOT(updateSymbolViewLevel()));

    connect(vkbWidget, SIGNAL(copyPasteRequest(CopyPasteState)),
            this, SLOT(sendCopyPaste(CopyPasteState)));
    connect(vkbWidget, SIGNAL(sendKeyEventRequest(const QKeyEvent &)),
            this, SLOT(sendKeyEvent(const QKeyEvent &)));
    connect(vkbWidget, SIGNAL(sendStringRequest(const QString &)),
            this, SLOT(sendString(const QString &)));

    if (!inputMethodCorrectionEngine->value().isNull()) {
        imCorrectionEngine = MImEngineWordsInterfaceFactory::instance()->createEngine(
                                inputMethodCorrectionEngine->value().toString());

        if (imCorrectionEngine) {
            initializeInputEngine();
            connect(inputMethodCorrectionSettings, SIGNAL(valueChanged()),
                    this, SLOT(synchronizeCorrectionSetting()));
        } else {
            qDebug() << __PRETTY_FUNCTION__ << "Failed to load correction engine"
                     << inputMethodCorrectionEngine->value().toString();
        }
    }

    feedbackPlayer = MComponentData::feedbackPlayer();

    backspaceTimer.setSingleShot(true);
    connect(&backspaceTimer, SIGNAL(timeout()), this, SLOT(autoBackspace()));

    // hide main layout when symbol view is shown to improve performance
    connect(symbolView, SIGNAL(opened()), vkbWidget, SLOT(hideMainArea()));
    connect(symbolView, SIGNAL(aboutToHide()), vkbWidget, SLOT(showMainArea()));
}

MKeyboardHost::~MKeyboardHost()
{
    hideLockOnInfoBanner();
    delete hardwareKeyboard;
    hardwareKeyboard = 0;
    delete vkbWidget;
    vkbWidget = 0;
    delete symbolView;
    symbolView = 0;
    delete correctionHost;
    correctionHost = 0;
    delete sceneWindow;
    sceneWindow = 0;
    delete vkbStyleContainer;
    vkbStyleContainer = 0;
    delete inputMethodCorrectionSettings;
    inputMethodCorrectionSettings = 0;
    if (imCorrectionEngine) {
        MImEngineWordsInterfaceFactory::instance()->deleteEngine(imCorrectionEngine);
        imCorrectionEngine = 0;
    }
    backspaceMode = NormalBackspace;
    backspaceTimer.stop();
    LayoutsManager::destroyInstance();
}

void MKeyboardHost::createCorrectionCandidateWidget()
{
    // construct correction candidate widget
    correctionHost = new MImCorrectionHost(sceneWindow);
    correctionHost->hideCorrectionWidget();

    connect(correctionHost, SIGNAL(regionUpdated(const QRegion &)),
            this, SLOT(handleRegionUpdate(const QRegion &)));
    connect(correctionHost, SIGNAL(candidateClicked(const QString &)),
            this, SLOT(commitString(const QString &)));
}


void MKeyboardHost::handleFocusChange(bool focusIn)
{
    haveFocus = focusIn;
    if (activeState == MInputMethod::OnScreen) {
        if (focusIn) {
            // reset the temporary shift state when focus is changed
            resetVirtualKeyboardShiftState();
        }
    } else {
        if (focusIn) {
            hardwareKeyboard->enable();
        } else {
            hardwareKeyboard->disable();
        }
        if (!focusIn) {
            inputMethodHost()->setInputModeIndicator(MInputMethod::NoIndicator);
        }
        hideLockOnInfoBanner();
    }

    if (focusIn) {
        sharedHandleArea->show();
    } else {
        sharedHandleArea->hide();
    }
}


void MKeyboardHost::show()
{
    QWidget *p = MPlainWindow::instance();

    if (p->nativeParentWidget()) {
        p = p->nativeParentWidget();
    }

    p->raise(); // make sure the window gets displayed

    if (activeState == MInputMethod::Hardware) {
        if (!hardwareKeyboard->symViewAvailable())
            symbolView->hideSymbolView();
    } else {
        //Onscreen state
        if (!vkbWidget->symViewAvailable())
            symbolView->hideSymbolView();
    }

    // FIXME: This is from time when toolbar was attached on top of vkb widget
    // and vbk was needed to be shown, even when the position was under visible
    // screen. This is no longer the case and the following showKeyboard() should
    // not be called on hardware mode. What does host::show() even mean in hw mode?
    vkbWidget->showKeyboard();

    // update input engine keyboard layout.
    updateEngineKeyboardLayout();

    updateCorrectionState();
}


void MKeyboardHost::hide()
{
    correctionHost->hideCorrectionWidget();
    symbolView->hideSymbolView();
    vkbWidget->hideKeyboard();
}


void MKeyboardHost::setPreedit(const QString &preeditString)
{
    preedit = preeditString;
    candidates.clear();
    if (imCorrectionEngine) {
        imCorrectionEngine->clearEngineBuffer();
        imCorrectionEngine->reselectString(preeditString);
        candidates = imCorrectionEngine->candidates();
        correctionHost->setCandidates(candidates);
    }
    updatePreedit(preedit, candidates.count());
}


void MKeyboardHost::update()
{
    bool valid = false;

    const bool hasSelection = inputMethodHost()->hasSelection(valid);
    if (valid) {
        imToolbar->setSelectionStatus(hasSelection);
    }

    const int type = inputMethodHost()->contentType(valid);
    if (valid) {
        hardwareKeyboard->setKeyboardType(static_cast<M::TextContentType>(type));
        vkbWidget->setKeyboardType(type);
    }

    updateAutoCapitalization();
    updateContext();

    const int inputMethodModeValue = inputMethodHost()->inputMethodMode(valid);
    if (valid) {
        inputMethodMode = inputMethodModeValue;
        hardwareKeyboard->setInputMethodMode(static_cast<M::InputMethodMode>(inputMethodMode));
        vkbWidget->setInputMethodMode(static_cast<M::InputMethodMode>(inputMethodMode));
        sharedHandleArea->setInputMethodMode(static_cast<M::InputMethodMode>(inputMethodMode));
    }
}


void MKeyboardHost::resetVirtualKeyboardShiftState()
{
    // reset the temporary shift state (shift on state set by user or auto capitalization,
    // besides capslocked)
    if (activeState == MInputMethod::OnScreen && vkbWidget->shiftStatus() != ModifierLockedState) {
        autoCapsTriggered = false;
        vkbWidget->setShiftState(ModifierClearState);
    }
}

void MKeyboardHost::updateAutoCapitalization()
{
    switch (activeState) {
    case MInputMethod::OnScreen:
        autoCapsEnabled = vkbWidget->autoCapsEnabled();
        break;
    default:
        autoCapsEnabled = hardwareKeyboard->autoCapsEnabled();
        break;
    }
    bool valid = false;
    const int type = inputMethodHost()->contentType(valid);
    autoCapsEnabled = (autoCapsEnabled
                       && valid
                       && (type != M::NumberContentType)
                       && (type != M::PhoneNumberContentType));
    autoCapsEnabled = (autoCapsEnabled
                       && inputMethodHost()->autoCapitalizationEnabled(valid)
                       && valid);
    autoCapsEnabled = (autoCapsEnabled
                       && inputMethodHost()->surroundingText(surroundingText, cursorPos));

    if (!autoCapsEnabled)
        return;

    // Capitalization is determined by preedit and Auto Capitalization.
    // If there are some preedit, it should be lower case.
    // Otherwise Auto Capitalization will turn on shift when (text entry capitalization option is ON):
    //   1. at the beginning of one paragraph
    //   2. after a sentence delimiter and one or more spaces
    static const QRegExp autoCapsTrigger("[" + AutoCapsSentenceDelimiters + "] +$");
    autoCapsTriggered = ((preedit.length() == 0)
                         && ((cursorPos == 0)
                             || ((cursorPos > 0)
                                 && (cursorPos <= surroundingText.length())
                                 && surroundingText.left(cursorPos).contains(autoCapsTrigger))));

    if ((activeState == MInputMethod::OnScreen)
        && (vkbWidget->shiftStatus() != ModifierLockedState)) {
        // FIXME: This will break the behaviour of keeping shift latched when shift+character occured.
        // We would really need a state machine for the shift state handling.
        vkbWidget->setShiftState(autoCapsTriggered ?
                                 ModifierLatchedState : ModifierClearState);
    } else if ((activeState == MInputMethod::Hardware) &&
               (hardwareKeyboard->modifierState(Qt::ShiftModifier) != ModifierLockedState)) {
        hardwareKeyboard->setAutoCapitalization(autoCapsTriggered);
    }
}

void MKeyboardHost::updateContext()
{
    if (!correctionEnabled || !preedit.isEmpty()) {
        return;
    }

    bool valid = false;
    const int type = inputMethodHost()->contentType(valid);
    if (!valid || (type == M::NumberContentType)
        || (type == M::PhoneNumberContentType)) {
        return;
    }

    if (inputMethodHost()->surroundingText(surroundingText, cursorPos)) {
        imCorrectionEngine->setContext(surroundingText, cursorPos);
    }
}

void MKeyboardHost::reset()
{
    qDebug() << __PRETTY_FUNCTION__;
    switch (activeState) {
    case MInputMethod::OnScreen:
        resetInternalState();
        break;
    case MInputMethod::Hardware:
        hardwareKeyboard->reset();
        break;
    case MInputMethod::Accessory:
        break;
    }
}


void MKeyboardHost::resetInternalState()
{
    backspaceMode = NormalBackspace;
    backspaceTimer.stop();
    preedit.clear();
    candidates.clear();
    correctionHost->reset();
    if (imCorrectionEngine)
        imCorrectionEngine->clearEngineBuffer();
}

void MKeyboardHost::prepareOrientationChange()
{
    if (rotationInProgress) {
        return;
    }
    rotationInProgress = true;

    symbolView->prepareToOrientationChange();
    vkbWidget->prepareToOrientationChange();
    correctionHost->prepareToOrientationChange();
}

void MKeyboardHost::finalizeOrientationChange()
{
    angle = MPlainWindow::instance()->orientationAngle();

    if (imToolbar) {
        // load proper layout
        imToolbar->finalizeOrientationChange();
    }

    vkbWidget->finalizeOrientationChange();
    symbolView->finalizeOrientationChange();
    if (sharedHandleArea) {
        sharedHandleArea->finalizeOrientationChange();
    }

    // Finalize candidate list after so its region will apply.
    correctionHost->finalizeOrientationChange();

    // If correction candidate widget was open we need to reposition it.
    if (correctionHost->isActive()) {
        bool success = false;
        const QRect rect = inputMethodHost()->preeditRectangle(success);
        QRect localRect;
        // Note: For Qt applications we don't have means to retrieve
        // the correct coordinates for pre-edit rectangle, so rect here
        // is null.
        if (success && !rect.isNull() && rotateRect(rect, localRect)) {
            correctionHost->setPosition(localRect);
            correctionHost->showCorrectionWidget(correctionHost->candidateMode());
        } else {
            correctionHost->hideCorrectionWidget();
        }
    }

    // reload keyboard layout for engine when orientation is changed
    engineLayoutDirty = true;
    if (vkbWidget->isVisible()) {
        updateEngineKeyboardLayout();
    }

    rotationInProgress = false;
}

bool MKeyboardHost::rotatePoint(const QPoint &screen, QPoint &window)
{
    bool res = true;

    switch (angle) {
    case M::Angle90:
        window.setX(screen.y());
        window.setY(displayWidth - screen.x());
        break;
    case M::Angle270:
        window.setX(displayHeight - screen.y());
        window.setY(screen.x());
        break;
    case M::Angle180:
        window.setX(displayWidth - screen.x());
        window.setY(displayHeight - screen.y());
        break;
    case M::Angle0:
        window.setX(screen.x());
        window.setY(screen.y());
        break;
    default:
        qCritical() << __FILE__ << __LINE__ << "Incorrect orientation" << angle;
        res = false;
        break;
    }
    return res;
}


bool MKeyboardHost::rotateRect(const QRect &screenRect, QRect &windowRect)
{
    bool res = true;

    if (!screenRect.isValid()) {
        windowRect = QRect();
        return false;
    }

    switch (angle) {
    case M::Angle90:
        windowRect.setRect(screenRect.y(),
                           displayWidth - screenRect.x() - screenRect.width(),
                           screenRect.height(), screenRect.width());
        break;
    case M::Angle270:
        windowRect.setRect(displayHeight - screenRect.y() - screenRect.height(),
                           screenRect.x(),
                           screenRect.height(), screenRect.width());
        break;
    case M::Angle180:
        windowRect.setRect(displayWidth - screenRect.x() - screenRect.width(),
                           displayHeight - screenRect.y() - screenRect.height(),
                           screenRect.width(), screenRect.height());
        break;
    case M::Angle0:
        windowRect = screenRect;
        break;
    default:
        qCritical() << __FILE__ << __LINE__ << " Incorrect orientation " << angle;
        windowRect = QRect();
        res = false;
        break;
    }
    return res;
}


void MKeyboardHost::handleMouseClickOnPreedit(const QPoint &mousePos, const QRect &preeditRect)
{
    Q_UNUSED(mousePos);
    Q_UNUSED(preeditRect);
    // Shows suggestion list when there are some candidates.
    // Even show suggestion list when there is only original input word in candidates.
    if (candidates.size() <= 0)
        return;

    correctionHost->showCorrectionWidget(MImCorrectionHost::WordListMode);
}


void MKeyboardHost::handleVisualizationPriorityChange(bool priority)
{
    vkbWidget->setTemporarilyHidden(priority);
    symbolView->setTemporarilyHidden(priority);
}


void MKeyboardHost::handleAppOrientationChange(int angle)
{
    // The application receiving input has changed its orientation. Let's change ours.
    MPlainWindow::instance()->setOrientationAngle(static_cast<M::OrientationAngle>(angle));
    this->angle = static_cast<M::OrientationAngle>(angle);
}


void MKeyboardHost::commitString(const QString &updatedString)
{
    if (candidates.count() > 1) {
        int suggestionIndex = candidates.indexOf(updatedString);
        if (suggestionIndex >= 0) {
            qDebug() << "save index:" << suggestionIndex;
            imCorrectionEngine->setSuggestedCandidateIndex(suggestionIndex);
        }
    }
    imCorrectionEngine->saveAndClearEngineBuffer();
    inputMethodHost()->sendCommitString(updatedString);
    preedit.clear();
}


void MKeyboardHost::doBackspace()
{
    // note: backspace shouldn't start accurate mode
    if (preedit.length() > 0) {
        if (!backspaceTimer.isActive()) {
            setPreedit(preedit.left(preedit.length() - 1));
        } else {
            resetInternalState();
            inputMethodHost()->sendCommitString("");
        }
    } else {
        const KeyEvent event("\b", QEvent::KeyPress, Qt::Key_Backspace,
                             KeyEvent::NotSpecial,
                             vkbWidget->shiftStatus() != ModifierClearState
                             ? Qt::ShiftModifier : Qt::NoModifier);
        inputMethodHost()->sendKeyEvent(event.toQKeyEvent(),
                                        MInputMethod::EventRequestEventOnly);
    }
    // Backspace toggles shift off if it's on (not locked)
    // except if autoCaps is on and cursor is at 0 position.
    if (vkbWidget->shiftStatus() == ModifierLatchedState
        && (!autoCapsEnabled || cursorPos != 0)) {
        vkbWidget->setShiftState(ModifierClearState);
    }
}

void MKeyboardHost::autoBackspace()
{
    backspaceTimer.start(BackspaceRepeatInterval); // Must restart before doBackspace
    doBackspace();
}

void MKeyboardHost::handleKeyPress(const KeyEvent &event)
{
    if (event.qtKey() == Qt::Key_Shift) {
        if (shiftHeldDown) {
            return; //ignore duplicated event
        }

        if (activeState == MInputMethod::OnScreen && enableMultiTouch) {
            shiftHeldDown = true;
        }
    }

    MInputMethod::EventRequestType requestType = MInputMethod::EventRequestSignalOnly;
    if (((inputMethodMode == M::InputMethodModeDirect)
         && (event.specialKey() == KeyEvent::NotSpecial))
        || (event.qtKey() == Qt::Key_plusminus)) { // plusminus key makes an exception

        requestType = MInputMethod::EventRequestBoth;

    } else if (event.qtKey() == Qt::Key_Backspace) {
        if (correctionHost->isActive()
            && correctionHost->candidateMode() == MImCorrectionHost::WordTrackerMode) {
            // hide word tracker when backspace key press
            correctionHost->hideCorrectionWidget();
            // WordTrackerBackspace mode: hide word tracker when backspace key press.
            // And remove preedit if holding backspace long enough. But does nothing
            // for backspace key release.
            startBackspace(WordTrackerBackspace);
        } else {
            startBackspace(NormalBackspace);
        }
    }

    inputMethodHost()->sendKeyEvent(event.toQKeyEvent(), requestType);
}

void MKeyboardHost::handleKeyRelease(const KeyEvent &event)
{
    if (event.qtKey() == Qt::Key_Shift) {
        if (!shiftHeldDown) {
            return; //ignore duplicated event
        }

        if (activeState == MInputMethod::OnScreen && enableMultiTouch) {
            shiftHeldDown = false;
        }
    }

    MInputMethod::EventRequestType requestType = MInputMethod::EventRequestSignalOnly;
    if (((inputMethodMode == M::InputMethodModeDirect)
         && (event.specialKey() == KeyEvent::NotSpecial))
        || (event.qtKey() == Qt::Key_plusminus)) { // plusminus key makes an exception

        requestType = MInputMethod::EventRequestBoth;

    } else if ((event.qtKey() == Qt::Key_Backspace)) {
        if ( backspaceTimer.isActive()) {
            backspaceTimer.stop();
            // If the backspace Mode is WordTrackerBackspace, don't need to
            // do backspace.
            if (backspaceMode != WordTrackerBackspace) {
                doBackspace();
            }
            backspaceMode = NormalBackspace;
        }
    }

    inputMethodHost()->sendKeyEvent(event.toQKeyEvent(), requestType);
}

void MKeyboardHost::updateReactionMaps()
{
    if (rotationInProgress) {
        return;
    }

    // Start by making everything transparent
    clearReactionMaps(MReactionMap::Transparent);

    QList<QGraphicsView *> views = MPlainWindow::instance()->scene()->views();
    foreach(QGraphicsView *view, views) {
        MReactionMap *reactionMap = MReactionMap::instance(view);

        if (!reactionMap) {
            continue;
        }

        // Candidates widget
        if (correctionHost && correctionHost->isActive()) {
            correctionHost->paintReactionMap(reactionMap, view);

            // Correction candidate widget occupies whole screen when it is WordListMode.
            if (correctionHost->candidateMode() == MImCorrectionHost::WordListMode)
                continue;
        }

        // Paint either symview or vkb widget reactive areas.
        if (symbolView && symbolView->isFullyVisible()) {
            symbolView->paintReactionMap(reactionMap, view);
        } else if (vkbWidget && vkbWidget->isFullyVisible()) {
            vkbWidget->paintReactionMap(reactionMap, view);
        }

        // Toolbar
        if (imToolbar && imToolbar->isVisible()) {
            imToolbar->paintReactionMap(reactionMap, view);
        }
    }
}

void MKeyboardHost::clearReactionMaps(const QString &clearValue)
{
    if (!MPlainWindow::instance()->scene()) {
        return;
    }

    foreach (QGraphicsView *view, MPlainWindow::instance()->scene()->views()) {
        MReactionMap *reactionMap = MReactionMap::instance(view);
        if (reactionMap) {
            reactionMap->setDrawingValue(clearValue, clearValue);
            reactionMap->setTransform(QTransform()); // Identity
            reactionMap->fillRectangle(0, 0, reactionMap->width(), reactionMap->height());
        }
    }
}

void MKeyboardHost::handleKeyClick(const KeyEvent &event)
{
    // Don't need send key events for Direct input mode here.
    // already send in handleKeyPress and handleKeyRelease.
    if (activeState == MInputMethod::Hardware && inputMethodMode != M::InputMethodModeDirect) {
        // In hardware keyboard mode symbol view is just another source for
        // events that will be handled by duihardwarekeyboard.  The native
        // modifiers may not be correct (depending on the current hwkbd modifier
        // state) but that doesn't matter.
        processKeyEvent(QEvent::KeyPress, event.qtKey(), event.modifiers(),
                        event.text(), false, 1, 0, 0);
        processKeyEvent(QEvent::KeyRelease, event.qtKey(), event.modifiers(),
                        event.text(), false, 1, 0, 0);
    } else if ((inputMethodMode != M::InputMethodModeDirect)) {
        handleTextInputKeyClick(event);
    }

    handleGeneralKeyClick(event);

    lastClickEvent = event;
}

void MKeyboardHost::handleGeneralKeyClick(const KeyEvent &event)
{
    if (event.qtKey() == Qt::Key_Shift) {
        switch (vkbWidget->shiftStatus()) {
        case ModifierLatchedState:
            // If current ShiftOn state is due to autocaps, go back to ShiftOff.
            // Otherwise, lock it.
            if (autoCapsTriggered) {
                vkbWidget->setShiftState(ModifierClearState);
            } else {
                vkbWidget->setShiftState(ModifierLockedState);
            }
            break;
        case ModifierClearState:
            vkbWidget->setShiftState(ModifierLatchedState);
            break;
        case ModifierLockedState:
            vkbWidget->setShiftState(ModifierClearState);
            break;
        }
        autoCapsTriggered = false;
    } else if (vkbWidget->shiftStatus() == ModifierLatchedState
               && (event.qtKey() != Qt::Key_Backspace)
               && (event.specialKey() != KeyEvent::Sym)
               && (event.specialKey() != KeyEvent::Switch)
               && (event.specialKey() != KeyEvent::LayoutMenu)
               && (!shiftHeldDown || autoCapsTriggered)) {
        // Any key except shift toggles shift off if it's on (not locked).
        // Exceptions are:
        // - backspace, toggles shift off is handled in doBackspace()
        // - sym, pressing sym key keeps current shift state
        // - switch, pressing switch key keeps current shift state
        // - menu, pressing menu key keeps current shift state
        // - shift, when held down don't bring level down, except with autocaps!
        //   note: For this we cannot use event.modifiers().testFlag(Qt::ShiftModifier)
        //         because it does not differentiate between latched+char and held down + char.
        vkbWidget->setShiftState(ModifierClearState);
    }

    if (event.specialKey() == KeyEvent::LayoutMenu) {
        showLayoutMenu();
    } else if (event.specialKey() == KeyEvent::Sym) {
        handleSymbolKeyClick();
    } else if (event.specialKey() == KeyEvent::Switch) {
        if (symbolView->isActive()) {
            symbolView->switchToNextPage();
        }
    }
}

void MKeyboardHost::handleLongKeyPress(const KeyEvent &event)
{
    if (event.qtKey() == Qt::Key_Space
        && correctionEnabled
        && correctionHost->isActive()
        && correctionHost->candidateMode() == MImCorrectionHost::WordTrackerMode
        && candidates.size() > 0) {
        // long tap space key when word tracker is visible will switch to word list.
        correctionHost->showCorrectionWidget(MImCorrectionHost::WordListMode);
    }
}

void MKeyboardHost::sendCommitStringOrReturnEvent(const KeyEvent &event) const
{
    // We send return as a normal key event instead of an input method event so
    // that it properly triggers returnPressed signal emission in MTextEdit and
    // QLineEdit (and possibly in other similar targets).
    if (event.qtKey() == Qt::Key_Return) {
        inputMethodHost()->sendKeyEvent(KeyEvent(event, QEvent::KeyPress).toQKeyEvent(), MInputMethod::EventRequestEventOnly);
        inputMethodHost()->sendKeyEvent(event.toQKeyEvent(), MInputMethod::EventRequestEventOnly);
    } else {
        inputMethodHost()->sendCommitString(event.text());
    }
}

void MKeyboardHost::handleTextInputKeyClick(const KeyEvent &event)
{
    // Discard KeyPress & Drop type of events.
    if (event.type() != QEvent::KeyRelease

        // Discard also special keys except cycleset which has text
        // that can be sent to input context.
        || (!(event.specialKey() == KeyEvent::NotSpecial
              || event.specialKey() == KeyEvent::CycleSet))

        // Finally, discard Qt backspace and plusminus key, which are handled in
        // handleKeyPress/Release.
        || (event.qtKey() == Qt::Key_Backspace)
        || (event.qtKey() == Qt::Key_plusminus)
        || (event.qtKey() == Qt::Key_Shift)) {

        return;
    }

    if (cycleKeyHandler->handleTextInputKeyClick(event)) {
        return;
    }
    QString text(event.text());

    if (text.isEmpty()) {
        return;
    }

    if (!correctionEnabled) {
        if (preedit.length() > 0) {
            // we just entered accurate mode. send the previous preedit stuff.
            inputMethodHost()->sendCommitString(preedit);
            if (imCorrectionEngine)
                imCorrectionEngine->clearEngineBuffer();
            preedit.clear();
        }

        sendCommitStringOrReturnEvent(event);

    } else if ((event.qtKey() == Qt::Key_Space) || (event.qtKey() == Qt::Key_Return) || (event.qtKey() == Qt::Key_Tab)) {
        // commit suggestion if correction candidate widget is visible and with popupMode
        // or ignore it if correction widget is visible and with suggestionlist mode
        // otherwise commit preedit
        if (event.qtKey() == Qt::Key_Space
            && correctionHost->isActive()) {
            if (correctionHost->candidateMode() == MImCorrectionHost::WordTrackerMode) {
                inputMethodHost()->sendCommitString(correctionHost->suggestion());
            } else {
                // ignore space click when word list is visible.
                return;
            }
        } else {
            inputMethodHost()->sendCommitString(preedit);
        }

        if (lastClickEvent.specialKey() != KeyEvent::CycleSet) {
            candidates.clear();
            correctionHost->reset();
        }
        // Send trailing space.
        sendCommitStringOrReturnEvent(event);

        imCorrectionEngine->clearEngineBuffer();
        preedit.clear();
    } else {
        // common case: just append stuff to current preedit
        preedit += text;

        candidates.clear();

        // send touch point to engine if not in symbol view
        // otherwise send character to engine.
        if ( !symbolView->isActive())
            imCorrectionEngine->tapKeyboard(event.pos(), vkbWidget->shiftStatus() != ModifierClearState, text.at(0));
        else
            imCorrectionEngine->appendCharacter(text.at(0));

        candidates = imCorrectionEngine->candidates();
        MImEngine::DictionaryType sourceDictionaryType = imCorrectionEngine->candidateSource(0);

        correctionHost->setCandidates(candidates);

        updatePreedit(preedit, candidates.count());

        // because the written word (preedit) should be on the top of candidate list.
        // So when there are some other candidates and the preedit is not a valid
        // dictionary word, show word tracker.
        if (candidates.count() > 1
            && sourceDictionaryType == MImEngine::DictionaryTypeInvalid) {
            bool success = false;
            const QRect rect = inputMethodHost()->preeditRectangle(success);
            QRect localRect;
            if (success && !rect.isNull() && rotateRect(rect, localRect)) {
                correctionHost->setPosition(localRect);
                correctionHost->showCorrectionWidget(MImCorrectionHost::WordTrackerMode);
            }
        } else {
            correctionHost->hideCorrectionWidget();
        }
    }
}

void MKeyboardHost::initializeInputEngine()
{
    // init correction engine
    // set language according current displayed language in virtual keyboard
    QString language = vkbWidget->layoutLanguage();

    if (language.isEmpty())
        language = DefaultInputLanguage;

    qDebug() << __PRETTY_FUNCTION__ << "- used language:" << language;

    if (imCorrectionEngine) {
        // TODO: maybe we should check return values here and in case of failure
        // be always in accurate mode, for example
        imCorrectionEngine->setLanguage(language, MImEngine::LanguagePriorityPrimary);
        engineLayoutDirty = true;
        updateEngineKeyboardLayout();
        synchronizeCorrectionSetting();
        imCorrectionEngine->disablePrediction();
        imCorrectionEngine->enableCompletion();
        imCorrectionEngine->setMaximumCandidates(MaximumErrorCorrectionCandidate);
        imCorrectionEngine->setExactWordPositionInList(MImEngine::ExactInListFirst);
    }
}

void MKeyboardHost::synchronizeCorrectionSetting()
{
    bool correction = inputMethodCorrectionSettings->value(DefaultInputMethodCorrectionSettingOption).toBool();

    if (!correction) {
        imCorrectionEngine->disableCorrection();
    } else {
        imCorrectionEngine->enableCorrection();
    }

    updateCorrectionState();
}


void MKeyboardHost::updateCorrectionState()
{
    if (activeState == MInputMethod::Hardware) {
        inputMethodHost()->setGlobalCorrectionEnabled(false);
        correctionEnabled = false;
    } else {
        if (!imCorrectionEngine) {
            inputMethodHost()->setGlobalCorrectionEnabled(false);
            correctionEnabled = false;
            return;
        }
        bool val = false;
        bool enabled = inputMethodHost()->correctionEnabled(val);
        if (val)
            correctionEnabled = enabled && imCorrectionEngine->correctionEnabled();
        else
            correctionEnabled = imCorrectionEngine->correctionEnabled();

        // info context the global correction option
        // TODO: should not put setGlobalCorrectionEnabled here, it will send correction setting
        // whenever focus changes. But have to at this moment, because im-uiserver start before
        // application, and there is no focus widget, no activateContext, calling
        // setGlobalCorrectionEnabled() at that time, can not record the setting.
        // Only after the application is running, this setGlobalCorrectionEnabled() can take effect
        inputMethodHost()->setGlobalCorrectionEnabled(imCorrectionEngine->correctionEnabled());
    }
}


void MKeyboardHost::userHide()
{
    vkbWidget->hideKeyboard();
    inputMethodHost()->notifyImInitiatedHiding();
}


void MKeyboardHost::sendCopyPaste(CopyPasteState action)
{
    switch (action) {
    case InputMethodCopy:
        inputMethodHost()->copy();
        break;
    case InputMethodPaste:
        inputMethodHost()->paste();
        break;
    default:
        qDebug() << __PRETTY_FUNCTION__ << "invalid action" << action;
        break;
    }
}

void MKeyboardHost::showLayoutMenu()
{
    inputMethodHost()->showSettings();
}

void MKeyboardHost::setRegionInfo(RegionList &regionStore,
                                  const QRegion &region,
                                  const QPointer<QObject> &widget)
{
    bool found = false;

    for (RegionList::iterator iterator = regionStore.begin();
         iterator != regionStore.end();
         ++iterator) {
        if (iterator->first == widget) {
            iterator->second = region;
            found = true;
            break;
        }
    }

    if (!found) {
        regionStore.append(ObjectRegionPair(widget, region));
    }
}

QRegion MKeyboardHost::combineRegion()
{
    return combineRegionImpl(widgetRegions, true);
}

QRegion MKeyboardHost::combineInputMethodArea()
{
    return combineRegionImpl(inputMethodAreaWidgetRegions, false);
}

QRegion MKeyboardHost::combineRegionImpl(const RegionList &regionStore,
                                         bool includeExtraInteractiveAreas)
{
    QRegion combinedRegion;

    foreach (const ObjectRegionPair &pair, regionStore) {
        if (pair.first) {
            combinedRegion |= pair.second;
        }
    }

    if (sharedHandleArea) {
        //add region occupied by sharedHandleArea
        combinedRegion = sharedHandleArea->addRegion(combinedRegion, includeExtraInteractiveAreas);
    }

    return combinedRegion;
}

void MKeyboardHost::handleRegionUpdate(const QRegion &region)
{
    QPointer<QObject> pointer(QObject::sender());

    setRegionInfo(widgetRegions, region, pointer);
    handleRegionUpdate();
}

void MKeyboardHost::handleRegionUpdate()
{
    inputMethodHost()->setScreenRegion(combineRegion());
    updateReactionMaps();
}

void MKeyboardHost::handleInputMethodAreaUpdate(const QRegion &region)
{
    QPointer<QObject> pointer(QObject::sender());

    setRegionInfo(inputMethodAreaWidgetRegions, region, pointer);
    handleInputMethodAreaUpdate();
}

void MKeyboardHost::handleInputMethodAreaUpdate()
{
    inputMethodHost()->setInputMethodArea(combineInputMethodArea());
}

void MKeyboardHost::switchPlugin(MInputMethod::SwitchDirection direction)
{
    inputMethodHost()->switchPlugin(direction);
}

void MKeyboardHost::sendKeyEvent(const QKeyEvent &key)
{
    inputMethodHost()->sendKeyEvent(key);
}

void MKeyboardHost::sendString(const QString &text)
{
    inputMethodHost()->sendCommitString(text);
}

void MKeyboardHost::sendStringFromToolbar(const QString &text)
{
    if (!preedit.isEmpty()) {
        sendString(preedit);
    }
    reset();
    sendString(text);
}

void MKeyboardHost::setToolbar(QSharedPointer<const MToolbarData> toolbar)
{
    if (toolbar && toolbar->isVisible()) {
        imToolbar->showToolbarWidget(toolbar);
    } else {
        imToolbar->hideToolbarWidget();
    }
}

void MKeyboardHost::processKeyEvent(QEvent::Type keyType, Qt::Key keyCode,
                                    Qt::KeyboardModifiers modifiers, const QString &text,
                                    bool autoRepeat, int count, quint32 nativeScanCode,
                                    quint32 nativeModifiers)
{
    if ((activeState != MInputMethod::Hardware) ||
        !hardwareKeyboard->filterKeyEvent(keyType, keyCode, modifiers, text,
                                          autoRepeat, count, nativeScanCode,
                                          nativeModifiers)) {
        inputMethodHost()->sendKeyEvent(QKeyEvent(keyType, keyCode, modifiers, text,
                                                  autoRepeat, count),
                                        MInputMethod::EventRequestEventOnly);
    }
}

void MKeyboardHost::handleClientChange()
{
    hardwareKeyboard->clientChanged();
    resetInternalState();
    hide(); // could do some quick hide also
}

void MKeyboardHost::switchContext(MInputMethod::SwitchDirection direction, bool enableAnimation)
{
    if (activeState == MInputMethod::OnScreen) {
        vkbWidget->switchLayout(direction, enableAnimation);
    }
}

void MKeyboardHost::setState(const QSet<MInputMethod::HandlerState> &state)
{
    if (state.isEmpty()) {
        return;
    }

    const MInputMethod::HandlerState actualState = *state.begin();
    if (activeState == actualState)
        return;

    if ((activeState == MInputMethod::OnScreen) && (preedit.length() > 0)) {
        inputMethodHost()->sendCommitString(preedit);
    }

    // Resets before changing the activeState to make sure clear.
    resetInternalState();
    activeState = actualState;

    // Keeps separate states for symbol view in OnScreen state and Hardware state
    if (activeState == MInputMethod::OnScreen) {
        hideLockOnInfoBanner();
        inputMethodHost()->setInputModeIndicator(MInputMethod::NoIndicator);

        disconnect(hardwareKeyboard, SIGNAL(deadKeyStateChanged(const QChar &)),
                   this, SLOT(handleHwKeyboardStateChanged()));
        disconnect(hardwareKeyboard, SIGNAL(modifiersStateChanged()),
                   this, SLOT(handleHwKeyboardStateChanged()));
        disconnect(hardwareKeyboard, SIGNAL(scriptChanged()),
                   this, SLOT(handleHwKeyboardStateChanged()));
        if (haveFocus) {
            hardwareKeyboard->disable();
        }
    } else {
        currentIndicatorDeadKey = false;
        connect(hardwareKeyboard, SIGNAL(deadKeyStateChanged(const QChar &)),
                this, SLOT(handleHwKeyboardStateChanged()));
        connect(hardwareKeyboard, SIGNAL(modifiersStateChanged()),
                this, SLOT(handleHwKeyboardStateChanged()));
        connect(hardwareKeyboard, SIGNAL(scriptChanged()),
                this, SLOT(handleHwKeyboardStateChanged()));
        if (haveFocus) {
            hardwareKeyboard->enable();
        }
    }

    symbolView->setKeyboardState(actualState);
    vkbWidget->setKeyboardState(actualState);
    updateCorrectionState();
    updateAutoCapitalization();
}

void MKeyboardHost::handleSymbolKeyClick()
{
    if (((activeState == MInputMethod::Hardware) && !hardwareKeyboard->symViewAvailable())
        || !vkbWidget->symViewAvailable()) {
        return;
    }

    // Toggle SymbolView.
    if (!symbolView->isActive()) {
        symbolView->showSymbolView();
        //give the symbolview right shift level(for hardware state)
        updateSymbolViewLevel();
    } else {
        symbolView->hideSymbolView();
    }
}

void MKeyboardHost::updateSymbolViewLevel()
{
    if (!symbolView->isActive())
        return;

    ModifierState shiftState = ModifierClearState;
    if (activeState == MInputMethod::OnScreen) {
        shiftState = vkbWidget->shiftStatus();
    } else {
        shiftState = hardwareKeyboard->modifierState(Qt::ShiftModifier);
    }
    symbolView->setShiftState(shiftState);
}

void MKeyboardHost::showSymbolView()
{
    symbolView->showSymbolView(SymbolView::FollowMouseShowMode);
    //give the symbolview right shift level(for hardware state)
    updateSymbolViewLevel();
}

MInputMethod::InputModeIndicator MKeyboardHost::deadKeyToIndicator(const QChar &key)
{
    switch (key.unicode()) {
    case 0x00b4:
        return MInputMethod::DeadKeyAcuteIndicator;
    case 0x02c7:
        return MInputMethod::DeadKeyCaronIndicator;
    case 0x005e:
        return MInputMethod::DeadKeyCircumflexIndicator;
    case 0x00a8:
        return MInputMethod::DeadKeyDiaeresisIndicator;
    case 0x0060:
        return MInputMethod::DeadKeyGraveIndicator;
    case 0x007e:
        return MInputMethod::DeadKeyTildeIndicator;
    default:
        return MInputMethod::NoIndicator;
    }
}

void MKeyboardHost::handleHwKeyboardStateChanged()
{
    // only change indicator state when there is focus is in a widget and state is Hardware.
    if (!haveFocus || activeState != MInputMethod::Hardware)
        return;

    const ModifierState shiftState = hardwareKeyboard->modifierState(Qt::ShiftModifier);
    const ModifierState fnState = hardwareKeyboard->modifierState(FnLevelModifier);
    const QString xkbLayout = LayoutsManager::instance().xkbLayout();
    const QString xkbVariant = LayoutsManager::instance().xkbVariant();

    const bool previousIndicatorDeadKey(currentIndicatorDeadKey);

    MInputMethod::InputModeIndicator indicatorState
        = deadKeyToIndicator(hardwareKeyboard->deadKeyState());

    currentIndicatorDeadKey = false;

    if (indicatorState != MInputMethod::NoIndicator) {
        currentIndicatorDeadKey = true;
    } else if (fnState == ModifierLockedState) {
        indicatorState = MInputMethod::NumAndSymLockedIndicator;

    } else if (fnState == ModifierLatchedState) {
        indicatorState = MInputMethod::NumAndSymLatchedIndicator;

    } else if (xkbLayout == "ara"
        && xkbVariant.isEmpty()) {
        indicatorState = MInputMethod::ArabicIndicator;

    } else if (xkbVariant.isEmpty() || xkbVariant == "latin") {
        indicatorState = MInputMethod::LatinLowerIndicator;
        if (shiftState == ModifierLockedState) {
            indicatorState = MInputMethod::LatinLockedIndicator;
        } else if (shiftState == ModifierLatchedState) {
            indicatorState = MInputMethod::LatinUpperIndicator;
        }

    } else if (xkbVariant == "cyrillic") {
        indicatorState = MInputMethod::CyrillicLowerIndicator;
        if (shiftState == ModifierLockedState) {
            indicatorState = MInputMethod::CyrillicLockedIndicator;
        } else if (shiftState == ModifierLatchedState) {
            indicatorState = MInputMethod::CyrillicUpperIndicator;
        }
    }

    inputMethodHost()->setInputModeIndicator(indicatorState);

    QString lockOnNotificationLabel;

    if (indicatorState == MInputMethod::LatinLockedIndicator
        || indicatorState == MInputMethod::CyrillicLockedIndicator) {
        //% "Caps lock on"
        lockOnNotificationLabel = qtTrId("qtn_hwkb_caps_lock");
    } else if (indicatorState == MInputMethod::NumAndSymLockedIndicator) {
        //% "Symbol lock on"
        lockOnNotificationLabel = qtTrId("qtn_hwkb_fn_lock");
    }
    if (!lockOnNotificationLabel.isEmpty()
        && !previousIndicatorDeadKey
        && hardwareKeyboard->keyboardType() != M::NumberContentType
        && hardwareKeyboard->keyboardType() != M::PhoneNumberContentType) {
        // notify the modifier is changed to locked state
        // number and phone number content type always force FN key to be locked,
        // don't need indicator lock notification.
        showLockOnInfoBanner(lockOnNotificationLabel);
    } else if (modifierLockOnBanner) {
        hideLockOnInfoBanner();
    }
}

void MKeyboardHost::showLockOnInfoBanner(const QString &notification)
{
    if (modifierLockOnBanner) {
        modifierLockOnBanner->setTitle(notification);
    } else {
        //TODO: discuss with UI designer whether we need to specify
        // the disappear time out.
        modifierLockOnBanner = new MBanner;
        modifierLockOnBanner->setTitle(notification);
        modifierLockOnBanner->appear(MSceneWindow::DestroyWhenDone);
    }
}

void MKeyboardHost::hideLockOnInfoBanner()
{
    if (modifierLockOnBanner) {
        modifierLockOnBanner->disappear();
    }
    modifierLockOnBanner = 0;
}

QList<MAbstractInputMethod::MInputMethodSubView>
MKeyboardHost::subViews(MInputMethod::HandlerState state) const
{
    QList<MAbstractInputMethod::MInputMethodSubView> sViews;
    if (state == MInputMethod::OnScreen) {
        QMap<QString, QString> selectedLayouts = LayoutsManager::instance().selectedLayouts();
        QMap<QString, QString>::const_iterator i = selectedLayouts.constBegin();
        while (i != selectedLayouts.constEnd()) {
            MAbstractInputMethod::MInputMethodSubView subView;
            subView.subViewId = i.key();
            subView.subViewTitle = i.value();
            sViews.append(subView);
            ++i;
        }
    }
    return sViews;
}

void MKeyboardHost::setActiveSubView(const QString &subViewId, MInputMethod::HandlerState state)
{
    if (state == MInputMethod::OnScreen) {
        const QStringList layoutFileList = LayoutsManager::instance().layoutFileList();
        int index = layoutFileList.indexOf(subViewId);
        vkbWidget->setLayout(index);
    }
}

QString MKeyboardHost::activeSubView(MInputMethod::HandlerState state) const
{
    if (state == MInputMethod::OnScreen) {
        // return the active vkb layout
        return vkbWidget->selectedLayout();
    } else {
        return QString();
    }
}

void MKeyboardHost::showLanguageNotification()
{
    if (activeState == MInputMethod::OnScreen && vkbWidget) {
        vkbWidget->requestLanguageNotification();
    }
}

void MKeyboardHost::handleVirtualKeyboardLayoutChanged(const QString &layout)
{
    // reset the temporary shift state when layout is changed
    resetVirtualKeyboardShiftState();
    if (symbolView) {
        symbolView->setLayout(layout);
    }

    initializeInputEngine();
    updateAutoCapitalization();
    emit activeSubViewChanged(layout);
}

void MKeyboardHost::updateEngineKeyboardLayout()
{
    if (!imCorrectionEngine || !engineLayoutDirty)
        return;

    if (activeState == MInputMethod::OnScreen) {
        imCorrectionEngine->setKeyboardLayoutKeys(vkbWidget->mainLayoutKeys());
    }
    engineLayoutDirty = false;
}

void MKeyboardHost::updatePreedit(const QString &string, int candidateCount)
{
    // preedit style type depends on candidateCount.
    // candidateCount     styleType
    //  0 or 1            PreeditNoCandidates
    //  1 or >1           PreeditDefault
    MInputMethod::PreeditFace face = MInputMethod::PreeditNoCandidates;
    if (candidateCount > 1) {
        face = MInputMethod::PreeditDefault;
    }

    inputMethodHost()->sendPreeditString(string, face);
}

void MKeyboardHost::startBackspace(MKeyboardHost::BackspaceMode mode)
{
    backspaceMode = mode;
    backspaceTimer.start(AutoBackspaceDelay);
}
