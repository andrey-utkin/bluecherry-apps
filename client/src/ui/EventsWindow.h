#ifndef EVENTSWINDOW_H
#define EVENTSWINDOW_H

#include <QWidget>

class DVRServersView;
class EventResultsView;
class QLabel;
class QBoxLayout;
class QDateTimeEdit;
class QComboBox;

class EventsWindow : public QWidget
{
    Q_OBJECT

public:
    explicit EventsWindow(QWidget *parent = 0);

protected:
    virtual void closeEvent(QCloseEvent *event);

private slots:
    void setStartDateEnabled(bool enabled);
    void levelFilterChanged();
    void updateResultTitle();

private:
    DVRServersView *m_sourcesView;
    QDateTimeEdit *m_startDate;
    QComboBox *m_levelFilter;
    QLabel *m_resultTitle;
    EventResultsView *m_resultsView;

    void createDateFilter(QBoxLayout *layout);
    QWidget *createLevelFilter();
    QWidget *createTypeFilter();

    QWidget *createResultTitle();
    QWidget *createResultsView();
    QWidget *createTimeline();
};

#endif // EVENTSWINDOW_H
