#include "QtHelpButtonInfo.h"

#include <QMessageBox>

QtHelpButtonInfo::QtHelpButtonInfo(const QString& title, const QString& text) : m_title(title), m_text(text) {}

void QtHelpButtonInfo::displayMessage(QWidget* messageBoxParent) {
  QMessageBox msgBox(messageBoxParent);
  msgBox.setWindowTitle(QStringLiteral("Sourcetrail"));
  msgBox.setIcon(QMessageBox::Information);
  msgBox.setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::LinksAccessibleByMouse);
  msgBox.setText("<b>" + m_title + "</b>");
  msgBox.setInformativeText(m_text);
  msgBox.setStandardButtons(QMessageBox::Ok);
  msgBox.setDefaultButton(QMessageBox::Ok);
  msgBox.exec();
}

QtHelpButtonInfo createErrorHelpButtonInfo() {
  return QtHelpButtonInfo(QStringLiteral("Fixing Errors"),
                          QStringLiteral("Please read this if your project is showing errors after indexing.<br />"
                                         "There are different types of errors:"
                                         "<ul>"
                                         "<li><b>Fatals</b> cause the indexer to stop. All or big parts of indexed "
                                         "information "
                                         "for the involved "
                                         "source file is missing. <b>Make sure to fix all fatals.</li>"
                                         "<li><b>Errors</b> are issues that the indexer considers as wrong code. Usually "
                                         "the "
                                         "indexer is able to "
                                         "recover from these errors. The indexed information of the involved source file "
                                         "may "
                                         "not be complete, but "
                                         "it is still useful.</li>"
                                         "</ul>"
                                         "You need to edit your Sourcetrail project and reindex it to fix errors. The "
                                         "displayed "
                                         "error messages are "
                                         "generated by Sourcetrail's language specific indexers:"
                                         "<ul>"
                                         "<li><b>C/C++</b> error messages are generated by the <b>clang compiler "
                                         "frontend</b>.</li>"
			"<li><b>Java</b> error messages are generated by the <b>Eclipse JDT "
			"library</b>.</li>"
			"<li><b>Python</b> error messages are generated by the <b>Jedi static analysis "
			"library</b>.</li>"
                                         "</ul>"
                                         "You should be able to find information about errors you are not familiar with "
                                         "online. "
                                         "<b>Double click</b> "
                                         "an error message in the table to select it for <b>copying</b>.<br />"));
}
