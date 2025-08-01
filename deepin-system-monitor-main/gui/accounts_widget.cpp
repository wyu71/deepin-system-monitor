#include "accounts_widget.h"
#include "ddlog.h"
#include <QWidget>
#include <QVBoxLayout>
#include <QPainterPath>

#include <QScroller>
#include <QScrollBar>
#include <QMessageBox>
#include <DApplication>
const QMargins ScrollAreaMargins(0, 0, 0, 0);
const QString LogoutDescription = "Log out the user may cause data loss, log out or not?";

using namespace DDLog;

AccountsWidget::AccountsWidget(QWidget *parent)
    : QWidget(parent), m_userModel(new AccountsInfoModel(this)), m_userItemModel(new QStandardItemModel(this)), m_userlistView(new UserListView(this))
{
    qCDebug(app) << "AccountsWidget constructor";
    m_currentUserType = m_userModel->getCurrentUserType();
    initUI();
    initConnection();
    addInfo(m_userModel);
    m_userlistView->resetStatus(m_userItemModel->index(0, 0));
}

AccountsWidget::~AccountsWidget()
{
    qCDebug(app) << "AccountsWidget destructor";
    if (m_userItemModel) {
        qCDebug(app) << "m_userItemModel is not null, clear it";
        m_userItemModel->clear();
        m_userItemModel->deleteLater();
        m_userItemModel = nullptr;
    }
    if (m_onlineIconList.size() > 0) {
        qCDebug(app) << "m_onlineIconList is not empty, clear it";
        m_onlineIconList.clear();
    }
}

void AccountsWidget::initUI()
{
    qCDebug(app) << "initUI";
    // disable auto fill frame background
    setAutoFillBackground(false);
    // set frame background role
    //    setBackgroundRole(DPalette::Window);
    // 禁用横向滚动条,防止内容被截断
    QVBoxLayout *mainContentLayout = new QVBoxLayout(this);
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    mainContentLayout->setMargin(0);
#else
    mainContentLayout->setContentsMargins(0, 0, 0, 0);
#endif
    mainContentLayout->setSpacing(0);

    mainContentLayout->addWidget(m_userlistView);

    m_userlistView->setFrameShape(QFrame::NoFrame);
    m_userlistView->setViewportMargins(ScrollAreaMargins);
    m_userlistView->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_userlistView->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_userlistView->setEditTriggers(QListView::NoEditTriggers);
    m_userlistView->setDragEnabled(false);
    m_userlistView->setIconSize(QSize(32, 32));
    setLayoutDirection(Qt::LeftToRight);
    m_userlistView->setModel(m_userItemModel);

    QScroller::grabGesture(m_userlistView->viewport(), QScroller::LeftMouseButtonGesture);
    QScroller *scroller = QScroller::scroller(m_userlistView);
    QScrollerProperties sp;
    sp.setScrollMetric(QScrollerProperties::VerticalOvershootPolicy, QScrollerProperties::OvershootAlwaysOff);
    scroller->setScrollerProperties(sp);
    setLayout(mainContentLayout);

    setFixedWidth(250);
    m_contextMenu = new DMenu(this);
}

void AccountsWidget::initConnection()
{
    qCDebug(app) << "Setting up signal connections";
    connect(m_userModel, &AccountsInfoModel::signalUserOnlineStatusUpdated, this, &AccountsWidget::onUpdateUserList);
    connect(m_userlistView, &QListView::clicked, this, &AccountsWidget::onItemClicked);
    connect(m_userlistView, &UserListView::signalRightButtonClicked, this, &AccountsWidget::onRightButtonClicked);
    connect(m_userlistView, &DListView::activated, m_userlistView, &QListView::clicked);

    /******************************* User Operation Menu on right Button Clicked******************************/

    //    auto *connectAction = m_contextMenu->addAction("Connect");
    //    connect(connectAction, &QAction::triggered, this, &AccountsWidget::onConnectTriggered);

    //    auto *disconnectAction = m_contextMenu->addAction("Disconnect");
    //    connect(disconnectAction, &QAction::triggered, this, &AccountsWidget::onDisconnectTriggered);

    //    auto *logoutAction = m_contextMenu->addAction("log out");
    //    connect(logoutAction, &QAction::triggered, this, &AccountsWidget::onLogoutTriggered);

    //    m_contextMenu->addSeparator();

    auto *EditAction = m_contextMenu->addAction(DApplication::translate("User.Account.Operation", "Edit account information"));
    connect(EditAction, &QAction::triggered, this, &AccountsWidget::onEditAccountTriggered);

    //    //判断当前用户是否为管理员用户,非管理员用户不展示连接和注销功能
    //    if (!(m_currentUserType == User::UserType::Administrator)) {
    //        connectAction->setVisible(false);
    //        logoutAction->setVisible(false);
    //    }

    /******************************* User Operation Menu on right Button Clicked******************************/
}

void AccountsWidget::onUpdateUserList()
{
    qCDebug(app) << "onUpdateUserList";
    //原来已连接现在注销的用户
    for (auto user : m_userList) {
        if (!m_userModel->userList().contains(user)) {
            qCDebug(app) << "Removing user:" << user->displayName();
            removeUser(user);
        }
    }
    //新增连接的用户
    qCInfo(app) << "Current user list size:" << m_userList.size() << "Model user list size:" << m_userModel->userList().size();
    for (auto user : m_userModel->userList()) {
        if (!m_userList.contains(user)) {
            qCDebug(app) << "Adding new user:" << user->displayName();
            addUser(user);
        }
    }
}

void AccountsWidget::addInfo(AccountsInfoModel *model)
{
    qCDebug(app) << "addInfo";
    //给账户列表添加用户
    for (auto user : model->userList()) {
        addUser(user);
    }
}

void AccountsWidget::addUser(User *user)
{
    qCDebug(app) << "addUser: " << user->displayName();
    //active
    m_userList << user;
    DStandardItem *item = new DStandardItem;
    item->setData(0, AccountsWidget::ItemDataRole);

    auto setTitelFunc = [=](int userType, DViewItemAction *subTitleAction) {
        subTitleAction->setText(userType == User::UserType::Administrator ? DApplication::translate("User.Account.Type", "Administrator") : DApplication::translate("User.Account.Type", "Standard User"));
    };

    /* 用户列表显示用户类型 */
    auto *subTitleAction = new DViewItemAction;
    setTitelFunc(user->userType(), subTitleAction);

    subTitleAction->setFontSize(DFontSizeManager::T8);
    subTitleAction->setTextColorRole(DPalette::TextTips);
    item->setTextActionList({ subTitleAction });

    //    DViewItemAction *onlineFlag = new DViewItemAction(Qt::AlignCenter | Qt::AlignRight, QSize(), QSize(), true);

    //    OnlineIcon *onlineIcon = new OnlineIcon(m_userlistView->viewport());
    //    onlineIcon->setFixedSize(8, 8);
    //    onlineFlag->setWidget(onlineIcon);
    //    item->setActionList(Qt::Edge::RightEdge, {onlineFlag});
    //    if (!user->online()) {
    //        onlineIcon->setColor(QColor(Qt::gray));
    //    }

    //    onlineFlag->setVisible(true);
    //    if (onlineFlag->widget()) {
    //        onlineFlag->widget()->setVisible(true);
    //    }

    //    m_onlineIconList << onlineIcon;

    m_userItemModel->appendRow(item);

    auto path = user->iconFile();
    path = path.startsWith("file://") ? QUrl(path).toLocalFile() : path;
    QPixmap pixmap = pixmapToRound(path);

    item->setIcon(QIcon(pixmap));
    item->setText(user->displayName());
    item->setToolTip(user->displayName());

    if (user->isCurrentUser()) {
        qCDebug(app) << "Moving current user to top of list:" << user->displayName();
        //如果是当前用户
        auto tttitem = m_userItemModel->takeRow(m_userItemModel->rowCount() - 1);
        Q_ASSERT(tttitem[0] == item);
        m_userItemModel->insertRow(0, item);

        m_userList.push_front(user);
        m_userList.pop_back();
    }
}

void AccountsWidget::removeUser(User *user)
{
    qCDebug(app) << "Removing user:" << user->displayName();
    m_userItemModel->removeRow(m_userList.indexOf(user));
    m_userList.removeOne(user);
    m_userlistView->update();
}

QPixmap AccountsWidget::pixmapToRound(const QPixmap &src)
{
    qCDebug(app) << "pixmapToRound";
    if (src.isNull()) {
        qCWarning(app) << "Attempted to convert null pixmap to round";
        return QPixmap();
    }

    auto pixmap = QPixmap(src);
    QSize size = pixmap.size();
    QPixmap mask(size);
    mask.fill(Qt::transparent);

    QPainter painter(&mask);
    painter.setRenderHint(QPainter::Antialiasing);

    QPainterPath path;
    path.addEllipse(0, 0, size.width(), size.height());
    painter.setClipPath(path);
    painter.drawPixmap(0, 0, pixmap);

    return mask;
}

QString AccountsWidget::getCurrentItemUserName()
{
    qCDebug(app) << "getCurrentItemUserName";
    //判断是否是全名
    for (auto *user : m_userList) {
        if (user->displayName() == m_userlistView->currentIndex().data().toString()) {
            qCDebug(app) << "Found user name:" << user->name() << "for display name:" << user->displayName();
            return user->name();
        }
    }
    qCDebug(app) << "Using display name as user name:" << m_userlistView->currentIndex().data().toString();
    return m_userlistView->currentIndex().data().toString();
}

void AccountsWidget::onItemClicked(const QModelIndex &index)
{
    qCDebug(app) << "onItemClicked: " << index;
    m_userlistView->resetStatus(index);
    Q_EMIT signalCurrentChanged();
}

// show process table view context menu on specified positon
void AccountsWidget::onRightButtonClicked(const QPoint &p)
{
    qCDebug(app) << "onRightButtonClicked: " << p;
    QPoint point = mapToGlobal(p);

    //    QString name = m_userlistView->indexAt(p).data().toString();
    //    //获取当前操作的用户对象
    //    getUserToBeOperated(name);

    //    //判断是否为当前用户，当前用户展示断开连接，
    //    if (m_userToBeOperated->isCurrentUser()) {
    //        m_contextMenu->actions().at(0)->setVisible(false);
    //        m_contextMenu->actions().at(1)->setVisible(true);
    //    } else {
    //        if (m_currentUserType == User::UserType::Administrator) {
    //            m_contextMenu->actions().at(0)->setVisible(true);
    //        }

    //        m_contextMenu->actions().at(1)->setVisible(false);
    //    }

    m_contextMenu->popup(point);
}

void AccountsWidget::getUserToBeOperated(const QString &userName)
{
    qCDebug(app) << "Getting user to be operated:" << userName;
    for (auto *user : m_userList) {
        if (user->displayName() == userName) {
            m_userToBeOperated = user;
            qCDebug(app) << "Found user:" << user->name();
            break;
        }
    }
}

void AccountsWidget::onConnectTriggered()
{
    qCDebug(app) << "onConnectTriggered";
    m_userModel->activateSessionByUserName(m_userToBeOperated->name());
}

void AccountsWidget::onDisconnectTriggered()
{
    qCDebug(app) << "onDisconnectTriggered";
    m_userModel->lockSessionByUserName(m_userToBeOperated->name());
}

void AccountsWidget::onLogoutTriggered()
{
    qCDebug(app) << "Logout triggered for user:" << m_userToBeOperated->name();
    // show confirm dialog
    KillProcessConfirmDialog dialog(this);
    dialog.setMessage(LogoutDescription);
    dialog.addButton(DApplication::translate("Cancel", "Cancel"), false);
    dialog.addButton(DApplication::translate("Logout", "Logout"), true,
                     DDialog::ButtonRecommend);
    dialog.exec();
    if (dialog.result() == QMessageBox::Ok) {
        qCDebug(app) << "User confirmed logout for:" << m_userToBeOperated->name();
        m_userModel->LogoutByUserName(m_userToBeOperated->name());
        //若为当前选中用户，进程列表切换到当前用户
        //todo
    } else {
        qCDebug(app) << "User cancelled logout for:" << m_userToBeOperated->name();
        return;
    }
}

void AccountsWidget::onEditAccountTriggered()
{
    qCDebug(app) << "onEditAccountTriggered";
    m_userModel->EditAccount();
}
