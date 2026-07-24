// SPDX-FileCopyrightText: 2017 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "widgets/BaseWidget.hpp"
#include "widgets/dialogs/KickLoginPage.hpp"

#include <QAction>
#include <QApplication>
#include <QButtonGroup>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QTabWidget>
#include <QtCore/QVariant>
#include <QToolButton>
#include <QVBoxLayout>

namespace chatterino {

class BasicLoginWidget : public QWidget
{
public:
    BasicLoginWidget();

    struct {
        QVBoxLayout layout;
        QHBoxLayout horizontalLayout;
        QPushButton loginButton;
        QPushButton pasteCodeButton;
        QLabel unableToOpenBrowserHelper;
    } ui_;
};

class AdvancedLoginWidget : public QWidget
{
public:
    AdvancedLoginWidget();

    void refreshButtons();

    struct {
        QVBoxLayout layout;

        QLabel instructionsLabel;

        QFormLayout formLayout;

        QLineEdit userIDInput;
        QLineEdit usernameInput;
        QLineEdit clientIDInput;
        QLineEdit oauthTokenInput;

        struct {
            QHBoxLayout layout;

            QPushButton addUserButton;
            QPushButton clearFieldsButton;
        } buttonUpperRow;
    } ui_;
};

class TwitchLoginWidget : public QWidget
{
public:
    TwitchLoginWidget();

private:
    QVBoxLayout layout_;
    BasicLoginWidget basic_;
    QToolButton advancedToggle_;
    AdvancedLoginWidget advanced_;
};

class RumbleLoginPage : public QWidget
{
public:
    RumbleLoginPage();

    void prepare();

private:
    void refresh();

    QVBoxLayout layout_;
    QLabel description_;
    QLabel status_;
    QProgressBar progress_;
    QPushButton loginButton_;
    bool preparationStarted_ = false;
};

class LoginDialog : public QDialog
{
public:
    LoginDialog(QWidget *parent);

private:
    struct {
        QVBoxLayout mainLayout;

        QTabWidget tabWidget;

        QDialogButtonBox buttonBox;

        TwitchLoginWidget twitch;

        KickLoginPage kick;
        RumbleLoginPage rumble;
    } ui_;
};

}  // namespace chatterino
