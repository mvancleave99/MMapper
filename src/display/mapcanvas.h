/************************************************************************
**
** Authors:   Ulf Hermann <ulfonk_mennhar@gmx.de> (Alve),
**            Marek Krejza <krejza@gmail.com> (Caligor),
**            Nils Schimmelmann <nschimme@gmail.com> (Jahara)
**
** This file is part of the MMapper project.
** Maintained by Nils Schimmelmann <nschimme@gmail.com>
**
** This program is free software; you can redistribute it and/or
** modify it under the terms of the GNU General Public License
** as published by the Free Software Foundation; either version 2
** of the License, or (at your option) any later version.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
**
** You should have received a copy of the GNU General Public License
** along with this program; if not, write to the:
** Free Software Foundation, Inc.
** 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
**
************************************************************************/

#ifndef MAPCANVAS_H
#define MAPCANVAS_H

#include "coordinate.h"
#include "mmapper2exit.h"

#include <set>
#include <vector>
#include <QMatrix4x4>
#include <QOpenGLDebugMessage>
#include <QOpenGLFunctions_1_0>
#include <QOpenGLWidget>

class QOpenGLTexture;
class MapData;
class Room;
class InfoMark;
class InfoMarksEditDlg;
class RoomSelection;
class ConnectionSelection;
class ParseEvent;
class Mmapper2Group;
class CGroupCharacter;
class RoomRecipient;
class PrespammedPath;
class Coordinate;
class QOpenGLDebugLogger;

class MapCanvas : public QOpenGLWidget, protected QOpenGLFunctions_1_0
{
    Q_OBJECT

public:
    MapCanvas(MapData *mapData,
              PrespammedPath *prespammedPath,
              Mmapper2Group *groupManager,
              QWidget *parent = 0);
    ~MapCanvas();

    QSize minimumSizeHint() const;
    QSize sizeHint() const;

    static float SCROLLFACTOR();
    float getDW() const;
    float getDH() const;

    enum CanvasMouseMode {
        CMM_NONE,
        CMM_MOVE,
        CMM_SELECT_ROOMS,
        CMM_SELECT_CONNECTIONS,
        CMM_CREATE_ROOMS,
        CMM_CREATE_CONNECTIONS,
        CMM_CREATE_ONEWAY_CONNECTIONS,
        CMM_EDIT_INFOMARKS
    };

    void drawRoom(const Room *room,
                  const std::vector<Room *> &rooms,
                  const std::vector<std::set<RoomRecipient *>> &locks);

public slots:
    void forceMapperToRoom();
    void createRoom();
    void onInfoMarksEditDlgClose();

    void setCanvasMouseMode(CanvasMouseMode);

    void setHorizontalScroll(int x);
    void setVerticalScroll(int y);
    void setScroll(int x, int y);
    void zoomIn();
    void zoomOut();
    void layerUp();
    void layerDown();

    void clearRoomSelection();
    void clearConnectionSelection();

    //void worldChanged();

    void dataLoaded();
    void moveMarker(const Coordinate &);

    void onMessageLogged(const QOpenGLDebugMessage &message);

signals:
    void onEnsureVisible(qint32 x, qint32 y);
    void onCenter(qint32 x, qint32 y);

    void mapMove(int dx, int dy);
    void setScrollBars(const Coordinate &ulf, const Coordinate &lrb);

    void log(const QString &, const QString &);

    void continuousScroll(qint8, qint8);

    void newRoomSelection(const RoomSelection *);
    void newConnectionSelection(ConnectionSelection *);

    void setCurrentRoom(uint id);
    void roomPositionChanged();

    //for main move/search algorithm
    void charMovedEvent(ParseEvent *);

protected:
    //void closeEvent(QCloseEvent *event);

    void initializeGL();
    void paintGL();

    void drawGroupCharacters();
    void drawCharacter(const Coordinate &c, const QColor &color);
    void drawRoomDoorName(const Room *sourceRoom,
                          uint sourceDir,
                          const Room *targetRoom,
                          uint targetDir);
    void resizeGL(int width, int height);
    void mousePressEvent(QMouseEvent *event);
    void mouseReleaseEvent(QMouseEvent *event);
    void mouseMoveEvent(QMouseEvent *event);
    void wheelEvent(QWheelEvent *event);

    void alphaOverlayTexture(QOpenGLTexture *texture);

    void drawConnection(const Room *leftRoom,
                        const Room *rightRoom,
                        ExitDirection connectionStartDirection,
                        ExitDirection connectionEndDirection,
                        bool oneway,
                        bool inExitFlags = true);
    void drawInfoMark(InfoMark *);
    void drawPathStart(const Coordinate &);
    bool drawPath(const Coordinate &sc, const Coordinate &dc, double &dx, double &dy, double &dz);
    void drawPathEnd(double dx, double dy, double dz);
    void drawFlow(const Room *room, const std::vector<Room *> &rooms, ExitDirection exitDirection);

    enum FontFormatFlags { FFF_NONE = 0, FFF_ITALICS = 1, FFF_UNDERLINE = 2 };
    void renderText(float x,
                    float y,
                    const QString &text,
                    const QColor &color = Qt::white,
                    uint fontFormatFlag = FFF_NONE,
                    double rotationAngle = 0.0);

private:
    QMatrix4x4 m_model, m_view, m_projection;
    QOpenGLDebugLogger *m_logger{};

    static QColor m_noFleeColor;

    QOpenGLTexture *m_terrainTextures[16]{};
    QOpenGLTexture *m_roadTextures[16]{};
    QOpenGLTexture *m_loadTextures[19]{};
    QOpenGLTexture *m_mobTextures[15]{};
    QOpenGLTexture *m_updateTexture;
    QOpenGLTexture *m_trailTextures[16]{};

    void makeGlLists();

    float m_scaleFactor{1.0f};
    int m_scrollX{0}, m_scrollY{0};
    qint16 m_currentLayer{0};
    float m_visibleX1{}, m_visibleY1{};
    float m_visibleX2{}, m_visibleY2{};

    bool m_mouseRightPressed{false};
    bool m_mouseLeftPressed{false};
    bool m_altPressed{false};
    bool m_ctrlPressed{false};

    CanvasMouseMode m_canvasMouseMode{CMM_MOVE};

    //mouse selection
    float m_selX1{}, m_selY1{}, m_selX2{}, m_selY2{};
    int m_selLayer1{}, m_selLayer2{};

    GLdouble m_moveX1backup{}, m_moveY1backup{};

    bool m_selectedArea{false}; //no area selected at start time
    const RoomSelection *m_roomSelection;

    bool m_roomSelectionMove{};
    bool m_roomSelectionMoveWrongPlace{};
    int m_roomSelectionMoveX{};
    int m_roomSelectionMoveY{};

    bool m_infoMarkSelection;

    ConnectionSelection *m_connectionSelection;

    Coordinate m_mapMove;

    MapData *m_data;
    PrespammedPath *m_prespammedPath;
    Mmapper2Group *m_groupManager;

    GLuint m_wall_north_gllist{};
    GLuint m_wall_south_gllist{};
    GLuint m_wall_east_gllist{};
    GLuint m_wall_west_gllist{};
    GLuint m_exit_up_gllist{};
    GLuint m_exit_down_gllist{};
    GLuint m_exit_up_transparent_gllist{};
    GLuint m_exit_down_transparent_gllist{};

    GLuint m_door_north_gllist{};
    GLuint m_door_south_gllist{};
    GLuint m_door_east_gllist{};
    GLuint m_door_west_gllist{};
    GLuint m_door_up_gllist{};
    GLuint m_door_down_gllist{};

    GLuint m_room_gllist{};
    GLuint m_room_selection_gllist{};
    GLuint m_room_selection_inner_gllist{};
    GLuint m_character_hint_gllist{};
    GLuint m_character_hint_inner_gllist{};

    GLuint m_flow_begin_gllist[6]{};
    GLuint m_flow_end_gllist[6]{};

    QFont *m_glFont;
    QFontMetrics *m_glFontMetrics;

    InfoMarksEditDlg *m_infoMarksEditDlg;

    int inline GLtoMap(double arg);
};
#endif
