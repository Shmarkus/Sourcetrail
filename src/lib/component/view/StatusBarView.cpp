#include "StatusBarView.h"

#include "StatusBarController.h"

StatusBarView::StatusBarView(ViewLayout* viewLayout) : View(viewLayout) {}

StatusBarView::~StatusBarView() = default;

std::string StatusBarView::getName() const {
  return "StatusBarView";
}

StatusBarController* StatusBarView::getStatusBarController() {
  return View::getController<StatusBarController>();
}
