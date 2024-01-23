#pragma once

#include "MessageBookmarkButtonState.h"
#include "MessageListener.h"
#include "View.h"

class BookmarkButtonsView
    : public View
    , public MessageListener<MessageBookmarkButtonState> {
public:
  BookmarkButtonsView(ViewLayout* viewLayout);
  ~BookmarkButtonsView() override;

  [[nodiscard]] std::string getName() const override;

  [[nodiscard]] Id getSchedulerId() const override;
  void setTabId(Id schedulerId);

  virtual void setCreateButtonState(const MessageBookmarkButtonState::ButtonState& state) = 0;

private:
  void handleMessage(MessageBookmarkButtonState* message) override;

  Id m_schedulerId;
};
