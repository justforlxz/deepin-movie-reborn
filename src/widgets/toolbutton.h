#ifndef _DMR_TOOLBUTTON_H
#define _DMR_TOOLBUTTON_H 

#include <QtWidgets>
#include <dimagebutton.h>

DWIDGET_USE_NAMESPACE

namespace dmr {

class VolumeButton: public DImageButton {
    Q_OBJECT
public:
    enum Level {
        Off,
        Low,
        Mid,
        High,
        Mute
    };

    VolumeButton(QWidget* parent = 0);
    void changeLevel(Level lv);

signals:
    void entered();
    void leaved();
    void requestVolumeUp();
    void requestVolumeDown();

protected:
    void enterEvent(QEvent *ev) override;
    void leaveEvent(QEvent *ev) override;
    void wheelEvent(QWheelEvent* wev) override;

private:
    QString _name;
    Level _lv {Mute};
};

}

#endif /* ifndef _DMR_TOOLBUTTON_H */

