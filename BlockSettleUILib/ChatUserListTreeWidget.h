#ifndef CHATUSERLISTTREEWIDGET_H
#define CHATUSERLISTTREEWIDGET_H

#include <QTreeWidget>
#include "ChatUserData.h"
#include "ChatUsersViewModel.h"
#include "ChatUserCategoryListView.h"

class ChatUserListTreeWidget : public QTreeWidget
{
   Q_OBJECT
public:
   explicit ChatUserListTreeWidget(QWidget *parent = nullptr);

signals:
   void userClicked(const QString &userId);

public slots:
   void onChatUserDataListChanged(const TChatUserDataListPtr &chatUserDataList);

private slots:
   void onUserListItemClicked(const QModelIndex &index);

private:
   void createCategories();
   void adjustListViewSize();
   ChatUserCategoryListView *listViewAt(int idx) const;

   ChatUsersViewModel *_friendUsersViewModel;
   ChatUsersViewModel *_nonFriendUsersViewModel;
};

#endif // CHATUSERLISTTREEWIDGET_H
