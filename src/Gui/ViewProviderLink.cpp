/****************************************************************************
 *   Copyright (c) 2017 Zheng, Lei (realthunder) <realthunder.dev@gmail.com>*
 *                                                                          *
 *   This file is part of the FreeCAD CAx development system.               *
 *                                                                          *
 *   This library is free software; you can redistribute it and/or          *
 *   modify it under the terms of the GNU Library General Public            *
 *   License as published by the Free Software Foundation; either           *
 *   version 2 of the License, or (at your option) any later version.       *
 *                                                                          *
 *   This library  is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of         *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the          *
 *   GNU Library General Public License for more details.                   *
 *                                                                          *
 *   You should have received a copy of the GNU Library General Public      *
 *   License along with this library; see the file COPYING.LIB. If not,     *
 *   write to the Free Software Foundation, Inc., 59 Temple Place,          *
 *   Suite 330, Boston, MA  02111-1307, USA                                 *
 *                                                                          *
 ****************************************************************************/

#include "PreCompiled.h"

#ifndef _PreComp_
# include <Inventor/nodes/SoSeparator.h>
# include <Inventor/nodes/SoSwitch.h>
# include <Inventor/nodes/SoTransform.h>
# include <Inventor/SoPickedPoint.h>
# include <Inventor/details/SoDetail.h>
# include <Inventor/misc/SoChildList.h>
# include <Inventor/nodes/SoMaterial.h>
# include <Inventor/nodes/SoDrawStyle.h>
# include <Inventor/nodes/SoShapeHints.h>
# include <Inventor/nodes/SoAnnotation.h>
# include <Inventor/actions/SoGetBoundingBoxAction.h>
# include <Inventor/nodes/SoPickStyle.h>
# include <Inventor/draggers/SoCenterballDragger.h>
# include <Inventor/nodes/SoSurroundScale.h>
# include <Inventor/nodes/SoCube.h>
#endif
#include <atomic>
#include <QApplication>
#include <QFileInfo>
#include <QMenu>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/bind.hpp>
#include <Base/Console.h>
#include <Base/PlacementPy.h>
#include <Base/MatrixPy.h>
#include <Base/BoundBoxPy.h>
#include <App/ComplexGeoData.h>
#include "Application.h"
#include "BitmapFactory.h"
#include "Document.h"
#include "Selection.h"
#include "MainWindow.h"
#include "ViewProviderLink.h"
#include "ViewProviderLinkPy.h"
#include "LinkViewPy.h"
#include "ViewProviderGeometryObject.h"
#include "View3DInventor.h"
#include "SoFCUnifiedSelection.h"
#include "SoFCCSysDragger.h"
#include "Control.h"
#include "TaskCSysDragger.h"

FC_LOG_LEVEL_INIT("App::Link",true,true)

using namespace Gui;

////////////////////////////////////////////////////////////////////////////

#ifdef FC_DEBUG
#define appendPath(_path,_node)  \
do{\
    if(_path->getLength()) {\
        SoNode * tail = _path->getTail();\
        const SoChildList * children = tail->getChildren();\
        if(!children || children->find((void *)_node)<0)\
            LINK_THROW(Base::RuntimeError,"LinkView: coin path error");\
    }\
    _path->append(_node);\
}while(0)
#else
#define appendPath(_path, _node) _path->append(_node)
#endif

////////////////////////////////////////////////////////////////////////////
class Gui::LinkInfo {

public:
    std::atomic<int> ref;

    typedef boost::BOOST_SIGNALS_NAMESPACE::scoped_connection Connection;
    Connection connChangeIcon;

    ViewProviderDocumentObject *pcLinked;
    std::set<Gui::LinkOwner*> links;

    typedef LinkInfoPtr Pointer;

    std::array<CoinPtr<SoSeparator>,LinkView::SnapshotMax> pcSnapshots;
    std::array<CoinPtr<SoSwitch>,LinkView::SnapshotMax> pcSwitches;
    CoinPtr<SoSwitch> pcLinkedSwitch;

    // for group type view providers
    CoinPtr<SoGroup> pcChildGroup;
    typedef std::map<SoNode *, Pointer> NodeMap;
    NodeMap nodeMap;

    std::map<qint64, QIcon> iconMap;

    static ViewProviderDocumentObject *getView(App::DocumentObject *obj) {
        if(obj && obj->getNameInDocument()) {
            Document *pDoc = Application::Instance->getDocument(obj->getDocument());
            if(pDoc) {
                ViewProvider *vp = pDoc->getViewProvider(obj);
                if(vp && vp->isDerivedFrom(ViewProviderDocumentObject::getClassTypeId()))
                    return static_cast<ViewProviderDocumentObject*>(vp);
            }
        }
        return 0;
    }

    static Pointer get(App::DocumentObject *obj, Gui::LinkOwner *owner) {
        return get(getView(obj),owner);
    }

    static Pointer get(ViewProviderDocumentObject *vp, LinkOwner *owner) {
        if(!vp) return Pointer();

        auto ext = vp->getExtensionByType<ViewProviderLinkObserver>(true);
        if(!ext) {
            ext = new ViewProviderLinkObserver();
            ext->initExtension(vp);
        }
        if(!ext->linkInfo) {
            // extension can be created automatically when restored from document,
            // with an empty linkInfo. So we need to check here.
            ext->linkInfo = Pointer(new LinkInfo(vp));
            ext->linkInfo->update();
        }
        if(owner)
            ext->linkInfo->links.insert(owner);
        return ext->linkInfo;
    }

    LinkInfo(ViewProviderDocumentObject *vp)
        :ref(0),pcLinked(vp) 
    {
        FC_LOG("new link to " << pcLinked->getObject()->getNameInDocument());
        connChangeIcon = vp->signalChangeIcon.connect(
                boost::bind(&LinkInfo::slotChangeIcon,this));
        vp->forceUpdate(true);
    }

    ~LinkInfo() {
    }

    bool checkName(const char *name) const {
        return isLinked() && strcmp(name,getLinkedName())==0;
    }

    void remove(LinkOwner *owner) {
        auto it = links.find(owner);
        if(it!=links.end())
            links.erase(it);
    }

    bool isLinked() const {
        return pcLinked && pcLinked->getObject() && 
           pcLinked->getObject()->getNameInDocument();
    }

    const char *getLinkedName() const {
        return pcLinked->getObject()->getNameInDocument();
    }

    const char *getLinkedNameSafe() const {
        if(isLinked())
            return getLinkedName();
        return "<nil>";
    }

    const char *getDocName() const {
        return pcLinked->getDocument()->getDocument()->getName();
    }

    void detach() {
        FC_LOG("link detach " << getLinkedNameSafe());
        auto me = LinkInfoPtr(this);
        while(links.size()) {
            auto link = *links.begin();
            links.erase(links.begin());
            link->unlink(me);
        }
        for(auto &node : pcSnapshots) {
            if(node) {
                node->removeAllChildren();
                node.reset();
            }
        }
        for(auto &node : pcSwitches) {
            if(node) {
                node->removeAllChildren();
                node.reset();
            }
        }
        pcLinkedSwitch.reset();
        if(pcChildGroup) {
            pcChildGroup->removeAllChildren();
            pcChildGroup.reset();
        }
        pcLinked = 0;
        connChangeIcon.disconnect();
    }

    void updateSwitch() {
        if(!isLinked() || !pcLinkedSwitch) return;
        int index = pcLinkedSwitch->whichChild.getValue();
        for(size_t i=0;i<pcSwitches.size();++i) {
            if(!pcSwitches[i]) 
                continue;
            int count = pcSwitches[i]->getNumChildren();
            if((index<0 && i==LinkView::SnapshotChild) || !count)
                pcSwitches[i]->whichChild = -1;
            else if(count>pcLinked->getDefaultMode())
                pcSwitches[i]->whichChild = pcLinked->getDefaultMode();
            else
                pcSwitches[i]->whichChild = 0;
        }
    }

    inline void addref() {
        ++ref;
    }

    inline void release(){
        int r = --ref;
        assert(r>=0);
        if(r==0) 
            delete this;
        else if(r==1) {
            if(pcLinked) {
                FC_LOG("link release " << getLinkedNameSafe());
                auto ext = pcLinked->getExtensionByType<ViewProviderLinkObserver>(true);
                if(ext) {
                    pcLinked->forceUpdate(false);
                    ext->extensionBeforeDelete();
                }
            }
        }
    }

    // VC2013 has trouble with template argument dependent lookup in
    // namespace. Have to put the below functions in global namespace.
    //
    // However, gcc seems to behave the oppsite, hence the conditional
    // compilation  here.
    //
#ifdef _MSC_VER
    friend void ::intrusive_ptr_add_ref(LinkInfo *px);
    friend void ::intrusive_ptr_release(LinkInfo *px);
#else
    friend inline void intrusive_ptr_add_ref(LinkInfo *px) { px->addref(); }
    friend inline void intrusive_ptr_release(LinkInfo *px) { px->release(); }
#endif

    SoSeparator *getSnapshot(int type, bool update=false) {
        if(type<0 || type>=LinkView::SnapshotMax)
            return 0;

        SoSeparator *root;
        if(!isLinked() || !(root=pcLinked->getRoot())) 
            return 0;

        auto &pcSnapshot = pcSnapshots[type];
        auto &pcModeSwitch = pcSwitches[type];
        if(pcSnapshot) {
            if(!update) return pcSnapshot;
        }else{
            pcSnapshot = new SoSeparator;
            pcModeSwitch = new SoSwitch;
        }

        pcLinkedSwitch.reset();

        pcSnapshot->removeAllChildren();
        pcModeSwitch->whichChild = -1;
        pcModeSwitch->removeAllChildren();

        auto childRoot = pcLinked->getChildRoot();

        if(type!=LinkView::SnapshotTransform)
            pcSnapshot->addChild(pcLinked->getTransformNode());

        for(int i=0,count=root->getNumChildren();i<count;++i) {
            SoNode *node = root->getChild(i);
            if(node==pcLinked->getTransformNode())
                continue;
            else if(node!=pcLinked->getModeSwitch()) {
                pcSnapshot->addChild(node);
                continue;
            }

            pcLinkedSwitch = static_cast<SoSwitch*>(node);

            pcSnapshot->addChild(pcModeSwitch);
            for(int i=0,count=pcLinkedSwitch->getNumChildren();i<count;++i) {
                auto child = pcLinkedSwitch->getChild(i);
                if(pcChildGroup && child==childRoot)
                    pcModeSwitch->addChild(pcChildGroup);
                else
                    pcModeSwitch->addChild(child);
            }
        }
        updateSwitch();
        return pcSnapshot;
    }

    void updateData(const App::Property *prop) {
        LinkInfoPtr me(this);
        for(auto link : links)
            link->onLinkedUpdateData(me,prop);
        update();
    }

    void update() {
        if(!isLinked()) return;
        
        if(pcLinked->isRestoring())
            return;

        if(pcLinked->getChildRoot()) {
            if(!pcChildGroup)
                pcChildGroup = new SoGroup;
            else
                pcChildGroup->removeAllChildren();

            NodeMap nodeMap;

            for(auto child : pcLinked->claimChildren3D()) {
                Pointer info = get(child,0);
                if(!info) continue;
                SoNode *node = info->getSnapshot(LinkView::SnapshotChild);
                if(!node) continue;
                nodeMap[node] = info;
                pcChildGroup->addChild(node);
            }

            // Use swap instead of clear() here to avoid potential link
            // destruction
            this->nodeMap.swap(nodeMap);
        }

        for(size_t i=0;i<pcSnapshots.size();++i) 
            if(pcSnapshots[i]) 
                getSnapshot(i,true);
    }

    bool getElementPicked(bool addname, int type, 
            const SoPickedPoint *pp, std::stringstream &str) const 
    {
        if(!pp || !isLinked() || !pcLinked->isSelectable())
            return false;

        if(addname) 
            str << getLinkedName() <<'.';
        
        auto pcSwitch = pcSwitches[type];
        if(pcChildGroup && pcSwitch && pcSwitch->whichChild.getValue()>=0 && 
            pcSwitch->getChild(pcSwitch->whichChild.getValue())==pcChildGroup)
        {
            SoPath *path = pp->getPath();
            int index = path->findNode(pcChildGroup);
            if(index<=0) return false;
            auto it = nodeMap.find(path->getNode(index+1));
            if(it==nodeMap.end()) return false;
            return it->second->getElementPicked(true,LinkView::SnapshotChild,pp,str);
        }else{
            std::string subname;
            if(!pcLinked->getElementPicked(pp,subname))
                return false;
            str<<subname;
        }
        return true;
    }

    static const char *checkSubname(App::DocumentObject *obj, const char *subname) {
#define CHECK_NAME(_name,_end) do{\
            if(!_name) return 0;\
            const char *_n = _name;\
            for(;*subname && *_n; ++subname,++_n)\
                if(*subname != *_n) break;\
            if(*_n || (*subname!=0 && *subname!=_end))\
                    return 0;\
            if(*subname == _end) ++subname;\
        }while(0)

        // if(subname[0] == '*') {
        //     ++subname;
        //     CHECK_NAME(obj->getDocument()->getName(),'*');
        // }
        CHECK_NAME(obj->getNameInDocument(),'.');
        return subname;
    }

    bool getDetail(bool checkname, int type, const char* subname, 
            SoDetail *&det, SoFullPath *path) const 
    {
        if(!isLinked()) return false;

        if(checkname) {
            subname = checkSubname(pcLinked->getObject(),subname);
            if(!subname) return false;
        }

        if(pcSnapshots[type]->findChild(pcSwitches[type]) < 0) {
            if(path) 
                appendPath(path,pcSnapshots[type]);
            // this is possible in case of editing, where the switch node
            // of the linked view object is temparaly removed from its root
            return false;
        }
        int len = 0;
        if(path) {
            len = path->getLength();
            appendPath(path,pcSnapshots[type]);
            appendPath(path,pcSwitches[type]);
        }
        if(*subname == 0) return true;

        auto pcSwitch = pcSwitches[type];
        if(!pcChildGroup || !pcSwitch || pcSwitch->whichChild.getValue()<0 ||
            pcSwitch->getChild(pcSwitch->whichChild.getValue())!=pcChildGroup)
        {
            return pcLinked->getDetailPath(subname,path,false,det);
        }
        if(path){
            appendPath(path,pcChildGroup);
            if(pcLinked->getChildRoot())
                type = LinkView::SnapshotChild;
            else
                type = LinkView::SnapshotVisible;
        }

        // Special handling of nodes with childRoot, especially geo feature
        // group. It's object hierarchy in the tree view (i.e. in subname) is
        // difference from its coin hierarchy. All objects under a geo feature
        // group is visually grouped directly under the group's childRoot,
        // even though some object has secondary hierarchy in subname. E.g.
        //
        // Body
        //   |--Pad
        //       |--Sketch
        //
        //  Both Sketch and Pad's coin nodes are grouped directly under Body as,
        //
        // Body
        //   |--Pad
        //   |--Sketch

        const char *dot = strchr(subname,'.');
        const char *nextsub = subname;
        if(!dot) return false;
        auto geoGroup = pcLinked->getObject();
        auto sobj = geoGroup;
        while(1) {
            std::string objname = std::string(nextsub,dot-nextsub+1);
            if(!geoGroup->getSubObject(objname.c_str())) {
                // this object is not found under the geo group, abort.
                break;
            }
            // Object found under geo group, remember this subname
            subname = nextsub;

            sobj = sobj->getSubObject(objname.c_str());
            if(!sobj) {
                FC_ERR("invalid sub name " << nextsub << " of object " << sobj->getNameInDocument());
                return false;
            }
            auto vp = Application::Instance->getViewProvider(sobj);
            if(!vp) {
                FC_ERR("cannot find view provider of " << sobj->getNameInDocument());
                return false;
            }
            if(vp->getChildRoot()) {
                // In case the children is also a geo group, it will visually
                // hold all of its own children, so stop going futher down.
                break;
            }
            // new style mapped sub-element
            if(Data::ComplexGeoData::isMappedElement(dot+1))
                break;
            auto next = strchr(dot+1,'.');
            if(!next) {
                // no dot any more, the following must be a sub-element
                break;
            }
            nextsub = dot+1;
            dot = next;
        }

        for(auto v : nodeMap) {
            if(v.second->getDetail(true,type,subname,det,path))
                return true;
        }
        if(path)
            path->truncate(len);
        return false;
    }

    void slotChangeIcon() {
        iconMap.clear();
        if(!isLinked()) 
            return;
        LinkInfoPtr me(this);
        for(auto link : links) 
            link->onLinkedIconChange(me);
    }

    QIcon getIcon(QPixmap px) {
        static int iconSize = -1;
        if(iconSize < 0) 
            iconSize = QApplication::style()->standardPixmap(QStyle::SP_DirClosedIcon).width();

        if(!isLinked())
            return QIcon();

        if(px.isNull()) 
            return pcLinked->getIcon();
        QIcon &iconLink = iconMap[px.cacheKey()];
        if(iconLink.isNull()) {
            QIcon icon = pcLinked->getIcon();
            iconLink = QIcon();
            iconLink.addPixmap(BitmapFactory().merge(icon.pixmap(iconSize, iconSize, QIcon::Normal, QIcon::Off),
                px,BitmapFactoryInst::BottomLeft), QIcon::Normal, QIcon::Off);
            iconLink.addPixmap(BitmapFactory().merge(icon.pixmap(iconSize, iconSize, QIcon::Normal, QIcon::On ),
                px,BitmapFactoryInst::BottomLeft), QIcon::Normal, QIcon::On);
        }
        return iconLink;
    }
};

#ifdef _MSC_VER
void intrusive_ptr_add_ref(Gui::LinkInfo *px){
    px->addref();
}

void intrusive_ptr_release(Gui::LinkInfo *px){
    px->release();
}
#endif

////////////////////////////////////////////////////////////////////////////////////

EXTENSION_TYPESYSTEM_SOURCE(Gui::ViewProviderLinkObserver,Gui::ViewProviderExtension);

ViewProviderLinkObserver::ViewProviderLinkObserver() {
    // TODO: any better way to get deleted automatically?
    m_isPythonExtension = true;
    initExtensionType(ViewProviderLinkObserver::getExtensionClassTypeId());
}

void ViewProviderLinkObserver::extensionBeforeDelete() {
    if(linkInfo) {
        linkInfo->detach();
        linkInfo.reset();
    }
}

void ViewProviderLinkObserver::extensionOnChanged(const App::Property *prop) {
    auto owner = dynamic_cast<ViewProviderDocumentObject*>(getExtendedContainer());
    if(!owner || !linkInfo) return;
    if(prop == &owner->Visibility || prop == &owner->DisplayMode)
        linkInfo->updateSwitch();
    else
        linkInfo->update();
}

void ViewProviderLinkObserver::extensionShow() {
    auto owner = dynamic_cast<ViewProviderDocumentObject*>(getExtendedContainer());
    if(owner && linkInfo)
        linkInfo->updateSwitch();
}

void ViewProviderLinkObserver::extensionHide() {
    auto owner = dynamic_cast<ViewProviderDocumentObject*>(getExtendedContainer());
    if(owner && linkInfo)
        linkInfo->updateSwitch();
}

void ViewProviderLinkObserver::extensionUpdateData(const App::Property *prop) {
    if(linkInfo && linkInfo->pcLinked && linkInfo->pcLinked->getObject() && 
       prop != &linkInfo->pcLinked->getObject()->Visibility) 
        linkInfo->updateData(prop);
}

void ViewProviderLinkObserver::extensionFinishRestoring() {
    if(linkInfo) {
        FC_TRACE("linked finish restoing");
        linkInfo->update();
    }
}

class LinkView::SubInfo : public LinkOwner {
public:
    LinkInfoPtr linkInfo;
    LinkView &handle;
    CoinPtr<SoSeparator> pcNode;
    CoinPtr<SoTransform> pcTransform;
    std::set<std::string> subElements;

    friend LinkView;

    SubInfo(LinkView &handle):handle(handle) {
        pcNode = new SoFCSelectionRoot;
        pcTransform = new SoTransform;
        pcNode->addChild(pcTransform);
    }

    ~SubInfo() {
        unlink();
        auto root = handle.getLinkRoot();
        if(root) {
            int idx = root->findChild(pcNode);
            if(idx>=0)
                root->removeChild(idx);
        }
    }

    virtual void onLinkedIconChange(LinkInfoPtr) override {
        if(handle.autoSubLink && handle.subInfo.size()==1)
            handle.onLinkedIconChange(handle.linkInfo);
    }

    virtual void unlink(LinkInfoPtr info=LinkInfoPtr()) override {
        (void)info;
        if(linkInfo) {
            linkInfo->remove(this);
            linkInfo.reset();
        }
        pcNode->removeAllChildren();
        pcNode->addChild(pcTransform);
    }

    void link(App::DocumentObject *obj) {
        if(isLinked() && linkInfo->pcLinked->getObject()==obj)
            return;
        unlink();
        linkInfo = LinkInfo::get(obj,this);
        if(linkInfo) 
            pcNode->addChild(linkInfo->getSnapshot(LinkView::SnapshotTransform));
    }

    bool isLinked() const{
        return linkInfo && linkInfo->isLinked();
    }
};

//////////////////////////////////////////////////////////////////////////////////

class LinkView::Element : public LinkOwner {
public:
    LinkInfoPtr linkInfo;
    LinkView &handle;
    CoinPtr<SoSwitch> pcSwitch;
    CoinPtr<SoFCSelectionRoot> pcRoot;
    CoinPtr<SoMaterial> pcMaterial;
    CoinPtr<SoTransform> pcTransform;

    friend LinkView;

    Element(LinkView &handle):handle(handle) {
        pcMaterial = handle.pcMaterial;
        pcTransform = new SoTransform;
        pcRoot = new SoFCSelectionRoot;
        pcRoot->addChild(pcMaterial);
        pcSwitch = new SoSwitch;
        pcSwitch->addChild(pcRoot);
        pcSwitch->whichChild = 0;
    }

    ~Element() {
        unlink();
        auto root = handle.getLinkRoot();
        if(root) {
            int idx = root->findChild(pcRoot);
            if(idx>=0)
                root->removeChild(idx);
        }
    }

    virtual void unlink(LinkInfoPtr info=LinkInfoPtr()) override{
        (void)info;
        if(linkInfo) {
            linkInfo->remove(this);
            linkInfo.reset();
        }
        pcRoot->removeAllChildren();
        pcRoot->addChild(pcMaterial);
    }

    void link(App::DocumentObject *obj) {
        if(isLinked() && linkInfo->pcLinked->getObject()==obj)
            return;
        unlink();
        linkInfo = LinkInfo::get(obj,this);
        if(isLinked()) 
            pcRoot->addChild(linkInfo->getSnapshot(handle.childType));
    }

    bool isLinked() const{
        return linkInfo && linkInfo->isLinked();
    }
};

///////////////////////////////////////////////////////////////////////////////////

TYPESYSTEM_SOURCE(Gui::LinkView,Base::BaseClass);

LinkView::LinkView()
    :nodeType(SnapshotTransform)
    ,childType((SnapshotType)-1),autoSubLink(true)
{
    pcLinkRoot = new SoFCSelectionRoot;
    pcMaterial = new SoMaterial;
    pcLinkRoot->addChild(pcMaterial);
}

LinkView::~LinkView() {
    unlink(linkInfo);
    unlink(linkOwner);
}

PyObject *LinkView::getPyObject(void)
{
    if (PythonObject.is(Py::_None()))
        PythonObject = Py::Object(new LinkViewPy(this),true);
    return Py::new_reference_to(PythonObject);
}

void LinkView::setInvalid(void) {
    if (!PythonObject.is(Py::_None())){
        Base::PyObjectBase* obj = (Base::PyObjectBase*)PythonObject.ptr();
        obj->setInvalid();
        obj->DecRef();
    }else
        delete this;
}

Base::BoundBox3d _getBoundBox(ViewProviderDocumentObject *vpd, SoNode *rootNode) {
    auto doc = vpd->getDocument();
    if(!doc) 
        LINK_THROW(Base::RuntimeError,"no document");
    Gui::MDIView* view = doc->getViewOfViewProvider(vpd);
    if(!view)
        LINK_THROW(Base::RuntimeError,"no view");
    
    Gui::View3DInventorViewer* viewer = static_cast<Gui::View3DInventor*>(view)->getViewer();
    SoGetBoundingBoxAction bboxAction(viewer->getSoRenderManager()->getViewportRegion());
    bboxAction.apply(rootNode);
    auto bbox = bboxAction.getBoundingBox();
    float minX,minY,minZ,maxX,maxY,maxZ;
    bbox.getMax().getValue(maxX,maxY,maxZ);
    bbox.getMin().getValue(minX,minY,minZ);
    return Base::BoundBox3d(minX,minY,minZ,maxX,maxY,maxZ);
}

Base::BoundBox3d LinkView::getBoundBox(ViewProviderDocumentObject *vpd) const {
    if(!vpd) {
        if(!linkOwner || !linkOwner->isLinked())
            LINK_THROW(Base::ValueError,"no ViewProvider");
        vpd = linkOwner->pcLinked;
    }
    return _getBoundBox(vpd,pcLinkRoot);
}

ViewProviderDocumentObject *LinkView::getOwner() const {
    if(linkOwner && linkOwner->isLinked())
        return linkOwner->pcLinked;
    return 0;
}

void LinkView::setOwner(ViewProviderDocumentObject *vpd) {
    unlink(linkOwner);
    linkOwner = LinkInfo::get(vpd,this);
}

bool LinkView::isLinked() const{
    return linkInfo && linkInfo->isLinked();
}

void LinkView::setDrawStyle(int style, double lineWidth, double pointSize) {
    if(!pcDrawStyle) {
        if(!style) 
            return;
        pcDrawStyle = new SoDrawStyle;
        pcDrawStyle->style = SoDrawStyle::FILLED;
        pcLinkRoot->insertChild(pcDrawStyle,0);
    }
    if(!style) {
        pcDrawStyle->setOverride(false);
        return;
    }
    pcDrawStyle->lineWidth = lineWidth;
    pcDrawStyle->pointSize = pointSize;
    switch(style) {
    case 2:
        pcDrawStyle->linePattern = 0xf00f;
        break;
    case 3:
        pcDrawStyle->linePattern = 0x0f0f;
        break;
    case 4:
        pcDrawStyle->linePattern = 0xff88;
        break;
    default:
        pcDrawStyle->linePattern = 0xffff;
    }
    pcDrawStyle->setOverride(true);
}

void LinkView::renderDoubleSide(bool enable) {
    if(enable) {
        if(!pcShapeHints) {
            pcShapeHints = new SoShapeHints;
            pcShapeHints->vertexOrdering = SoShapeHints::UNKNOWN_ORDERING;
            pcShapeHints->shapeType = SoShapeHints::UNKNOWN_SHAPE_TYPE;
            pcLinkRoot->insertChild(pcShapeHints,0);
        }
        pcShapeHints->setOverride(true);
    }else if(pcShapeHints)
        pcShapeHints->setOverride(false);
}

void LinkView::setMaterial(int index, const App::Material *material) {
    auto pcMat = pcMaterial;
    if(index < 0) {
        if(!material) {
            pcMaterial->setOverride(false);
            return;
        }
    }else if(index >= (int)nodeArray.size())
        LINK_THROW(Base::ValueError,"LinkView: material index out of range");
    else {
        auto &info = *nodeArray[index];
        if(info.pcMaterial == pcMaterial) {
            if(!material) 
                return;
            info.pcMaterial = pcMat = new SoMaterial;
            info.pcRoot->replaceChild(pcMaterial,pcMat);
        }else if (!material) {
            info.pcRoot->replaceChild(info.pcMaterial,pcMaterial);
            info.pcMaterial = pcMaterial;
            return;
        }else
            pcMat = info.pcMaterial;
    }

    pcMat->setOverride(true);

    const App::Material &Mat = *material;
    pcMat->ambientColor.setValue(Mat.ambientColor.r,Mat.ambientColor.g,Mat.ambientColor.b);
    pcMat->specularColor.setValue(Mat.specularColor.r,Mat.specularColor.g,Mat.specularColor.b);
    pcMat->emissiveColor.setValue(Mat.emissiveColor.r,Mat.emissiveColor.g,Mat.emissiveColor.b);
    pcMat->shininess.setValue(Mat.shininess);
    pcMat->diffuseColor.setValue(Mat.diffuseColor.r,Mat.diffuseColor.g,Mat.diffuseColor.b);
    pcMat->transparency.setValue(Mat.transparency);
}

void LinkView::setLink(App::DocumentObject *obj, const std::vector<std::string> &subs) {
    if(!isLinked() || linkInfo->pcLinked->getObject()!=obj) {
        unlink(linkInfo);
        linkInfo = LinkInfo::get(obj,this);
        if(!linkInfo) 
            return;
    }
    subInfo.clear();
    for(const auto &sub : subs) {
        if(sub.empty()) continue;
        const char *subelement = Data::ComplexGeoData::findElementName(sub.c_str());
        std::string subname = sub.substr(0,subelement-sub.c_str());
        auto it = subInfo.find(subname);
        if(it == subInfo.end()) {
            it = subInfo.insert(std::make_pair(subname,std::unique_ptr<SubInfo>())).first;
            it->second.reset(new SubInfo(*this));
        }
        if(subelement[0])
            it->second->subElements.insert(subelement);
    }
    updateLink();
}

void LinkView::setTransform(SoTransform *pcTransform, const Base::Matrix4D &mat) {
#if 1
    double dMtrx[16];
    mat.getGLMatrix(dMtrx);
    pcTransform->setMatrix(SbMatrix(dMtrx[0], dMtrx[1], dMtrx[2],  dMtrx[3],
                                    dMtrx[4], dMtrx[5], dMtrx[6],  dMtrx[7],
                                    dMtrx[8], dMtrx[9], dMtrx[10], dMtrx[11],
                                    dMtrx[12],dMtrx[13],dMtrx[14], dMtrx[15]));
#else
    // extract scale factor from colum vector length
    double sx = Base::Vector3d(mat[0][0],mat[1][0],mat[2][0]).Sqr();
    double sy = Base::Vector3d(mat[0][1],mat[1][1],mat[2][1]).Sqr();
    double sz = Base::Vector3d(mat[0][2],mat[1][2],mat[2][2]).Sqr();
    bool bx,by,bz;
    if((bx=fabs(sx-1.0)>=1e-10))
        sx = sqrt(sx);
    else
        sx = 1.0;
    if((by=fabs(sy-1.0)>=1e-10))
        sy = sqrt(sy);
    else
        sy = 1.0;
    if((bz=fabs(sz-1.0)>=1e-10))
        sz = sqrt(sz);
    else
        sz = 1.0;
    // TODO: how to deal with negative scale?
    pcTransform->scaleFactor.setValue(sx,sy,sz);

    Base::Matrix4D matRotate;
    if(bx) {
        matRotate[0][0] = mat[0][0]/sx;
        matRotate[1][0] = mat[1][0]/sx;
        matRotate[2][0] = mat[2][0]/sx;
    }else{
        matRotate[0][0] = mat[0][0];
        matRotate[1][0] = mat[1][0];
        matRotate[2][0] = mat[2][0];
    }
    if(by) {
        matRotate[0][1] = mat[0][1]/sy;
        matRotate[1][1] = mat[1][1]/sy;
        matRotate[2][1] = mat[2][1]/sy;
    }else{
        matRotate[0][1] = mat[0][1];
        matRotate[1][1] = mat[1][1];
        matRotate[2][1] = mat[2][1];
    }
    if(bz) {
        matRotate[0][2] = mat[0][2]/sz;
        matRotate[1][2] = mat[1][2]/sz;
        matRotate[2][2] = mat[2][2]/sz;
    }else{
        matRotate[0][2] = mat[0][2];
        matRotate[1][2] = mat[1][2];
        matRotate[2][2] = mat[2][2];
    }

    Base::Rotation rot(matRotate);
    pcTransform->rotation.setValue(rot[0],rot[1],rot[2],rot[3]);
    pcTransform->translation.setValue(mat[0][3],mat[1][3],mat[2][3]);
    pcTransform->center.setValue(0.0f,0.0f,0.0f);
#endif
}

void LinkView::setSize(int _size) {
    size_t size = _size<0?0:(size_t)_size;
    if(childType<0 && size==nodeArray.size()) 
        return;
    resetRoot();
    if(!size || childType>=0) {
        nodeArray.clear();
        nodeMap.clear();
        childType = (SnapshotType)-1;
        if(!size && childType<0) {
            if(pcLinkedRoot)
                pcLinkRoot->addChild(pcLinkedRoot);
            return;
        }
    }
    if(size<nodeArray.size()) {
        for(size_t i=size;i<nodeArray.size();++i)
            nodeMap.erase(nodeArray[i]->pcSwitch);
        nodeArray.resize(size);
    }
    for(auto &info : nodeArray)
        pcLinkRoot->addChild(info->pcSwitch);

    while(nodeArray.size()<size) {
        nodeArray.push_back(std::unique_ptr<Element>(new Element(*this)));
        auto &info = *nodeArray.back();
        info.pcRoot->addChild(info.pcTransform);
        if(pcLinkedRoot)
            info.pcRoot->addChild(pcLinkedRoot);
        pcLinkRoot->addChild(info.pcSwitch);
        nodeMap.insert(std::make_pair(info.pcSwitch,(int)nodeArray.size()-1));
    }
}

void LinkView::resetRoot() {
    pcLinkRoot->removeAllChildren();
    if(pcTransform)
        pcLinkRoot->addChild(pcTransform);
    if(pcShapeHints)
        pcLinkRoot->addChild(pcShapeHints);
    if(pcDrawStyle)
        pcLinkRoot->addChild(pcDrawStyle);
    if(pcMaterial)
        pcLinkRoot->addChild(pcMaterial);
}

void LinkView::setChildren(const std::vector<App::DocumentObject*> &children,
        const boost::dynamic_bitset<> &vis, SnapshotType type) 
{
    if(children.empty()) {
        if(nodeArray.size()) {
            nodeArray.clear();
            nodeMap.clear();
            childType = (SnapshotType)-1;
            resetRoot();
            if(pcLinkedRoot)
                pcLinkRoot->addChild(pcLinkedRoot);
        }
        return;
    }

    if(type<0 || type>=SnapshotMax)
        LINK_THROW(Base::ValueError,"invalid children type");

    resetRoot();

    if(childType<0)
        nodeArray.clear();
    childType = type;

    if(nodeArray.size() > children.size())
        nodeArray.resize(children.size());
    nodeArray.reserve(children.size());
    for(size_t i=0;i<children.size();++i) {
        auto obj = children[i];
        if(nodeArray.size()<=i)
            nodeArray.push_back(std::unique_ptr<Element>(new Element(*this)));
        auto &info = *nodeArray[i];
        info.pcSwitch->whichChild = (vis.size()<=i||vis[i])?0:-1;
        info.link(obj);
    }
    nodeMap.clear();
    for(size_t i=0;i<nodeArray.size();++i) {
        auto &info = *nodeArray[i];
        pcLinkRoot->addChild(info.pcSwitch);
        nodeMap.insert(std::make_pair(info.pcSwitch,i));
    }
}

std::vector<ViewProviderDocumentObject*> LinkView::getChildren() const {
    std::vector<ViewProviderDocumentObject*> ret;
    for(auto &info : nodeArray) {
        if(info->isLinked())
            ret.push_back(info->linkInfo->pcLinked);
    }
    return ret;
}

void LinkView::setTransform(int index, const Base::Matrix4D &mat) {
    if(index<0) {
        if(!pcTransform) {
            pcTransform = new SoTransform;
            pcLinkRoot->insertChild(pcTransform,0);
        }
        setTransform(pcTransform,mat);
        return;
    }
    if(index<0 || index>=(int)nodeArray.size())
        LINK_THROW(Base::ValueError,"LinkView: index out of range");
    setTransform(nodeArray[index]->pcTransform,mat);
}

void LinkView::setElementVisible(int idx, bool visible) {
    if(idx>=0 && idx<(int)nodeArray.size())
        nodeArray[idx]->pcSwitch->whichChild = visible?0:-1;
}

bool LinkView::isElementVisible(int idx) const {
    if(idx>=0 && idx<(int)nodeArray.size())
        return nodeArray[idx]->pcSwitch->whichChild.getValue()>=0;
    return false;
}

ViewProviderDocumentObject *LinkView::getLinkedView() const {
    auto link = linkInfo;
    if(autoSubLink && subInfo.size()==1) 
        link = subInfo.begin()->second->linkInfo;
    return link?link->pcLinked:0;
}

std::vector<std::string> LinkView::getSubNames() const {
    std::vector<std::string> ret;
    for(auto &v : subInfo) {
        if(v.second->subElements.empty()) {
            ret.push_back(v.first);
            continue;
        }
        for(auto &s : v.second->subElements)
            ret.push_back(v.first+s);
    }
    return ret;
}

void LinkView::setNodeType(SnapshotType type, bool sublink) {
    autoSubLink = sublink;
    if(nodeType==type) return;
    if(type>=SnapshotMax || 
       (type<0 && type!=SnapshotContainer && type!=SnapshotContainerTransform))
        LINK_THROW(Base::ValueError,"LinkView: invalid node type");

    if(nodeType>=0 && type<0) {
        if(pcLinkedRoot) {
            SoSelectionElementAction action(SoSelectionElementAction::None,true);
            action.apply(pcLinkedRoot);
        }
        replaceLinkedRoot(CoinPtr<SoSeparator>(new SoFCSelectionRoot));
    }else if(nodeType<0 && type>=0) {
        if(isLinked())
            replaceLinkedRoot(linkInfo->getSnapshot(type));
        else
            replaceLinkedRoot(0);
    }
    nodeType = type;
    updateLink();
}

void LinkView::replaceLinkedRoot(SoSeparator *root) {
    if(root==pcLinkedRoot) 
        return;
    if(nodeArray.empty()) {
        if(pcLinkedRoot && root) 
            pcLinkRoot->replaceChild(pcLinkedRoot,root);
        else if(root)
            pcLinkRoot->addChild(root);
        else 
            resetRoot();
    }else if(childType<0) {
        if(pcLinkedRoot && root) {
            for(auto &info : nodeArray)
                info->pcRoot->replaceChild(pcLinkedRoot,root);
        }else if(root) {
            for(auto &info : nodeArray)
                info->pcRoot->addChild(root);
        }else{
            for(auto &info : nodeArray)
                info->pcRoot->removeChild(pcLinkedRoot);
        }
    }
    pcLinkedRoot = root;
}

void LinkView::onLinkedIconChange(LinkInfoPtr info) {
    if(info==linkInfo && info!=linkOwner && linkOwner && linkOwner->isLinked())
        linkOwner->pcLinked->signalChangeIcon();
}

void LinkView::onLinkedUpdateData(LinkInfoPtr info, const App::Property *prop) {
    if(info!=linkInfo || !linkOwner || !linkOwner->isLinked() || info==linkOwner)
        return;
    auto ext = linkOwner->pcLinked->getObject()->getExtensionByType<App::LinkBaseExtension>(true);
    if (ext && !(prop->getType() & App::Prop_Output) && 
            !prop->testStatus(App::Property::Output)) 
    {
        // propagate the signalChangedObject to potentially multiple levels
        // of links, to inform tree view of children change, and other
        // parent objects about the change. But we need to be careful to not
        // touch the object if the property of change is marked as output.
        ext->_LinkRecomputed.touch();
    }else{
        // In case the owner object does not have link extension, here is a
        // trick to link the signalChangedObject from linked object to the
        // owner
        linkOwner->pcLinked->getDocument()->signalChangedObject(
                *linkOwner->pcLinked,linkOwner->pcLinked->getObject()->Label);
    }
}

void LinkView::updateLink() {
    if(!isLinked())
        return;

    if(linkOwner && linkOwner->isLinked() && linkOwner->pcLinked->isRestoring()) {
        FC_TRACE("restoring '" << linkOwner->pcLinked->getObject()->getNameInDocument() << "'");
        return;
    }

    // TODO: is it a good idea to clear any selection here?
    pcLinkRoot->resetContext();

    if(nodeType >= 0) {
        replaceLinkedRoot(linkInfo->getSnapshot(nodeType));
        return;
    }

    // rebuild link sub objects tree
    CoinPtr<SoSeparator> linkedRoot = pcLinkedRoot;
    if(!linkedRoot)
        linkedRoot = new SoFCSelectionRoot;
    else{
        SoSelectionElementAction action(SoSelectionElementAction::None,true);
        action.apply(linkedRoot);
        linkedRoot->removeAllChildren();
    }

    CoinPtr<SoFullPath> path;
    auto obj = linkInfo->pcLinked->getObject();
    for(auto &v : subInfo) {
        auto &sub = *v.second;
        Base::Matrix4D mat;
        App::DocumentObject *sobj = obj->getSubObject(
                v.first.c_str(), 0, &mat, nodeType==SnapshotContainer);
        if(!sobj) {
            sub.unlink();
            continue;
        }
        sub.link(sobj);
        linkedRoot->addChild(sub.pcNode);
        setTransform(sub.pcTransform,mat);

        if(sub.subElements.size()) {
            if(!path) {
                path = static_cast<SoFullPath*>(new SoPath(10));
                appendPath(path,linkedRoot);
            }
            path->truncate(1);
            appendPath(path,sub.pcNode);
            SoSelectionElementAction action(SoSelectionElementAction::Append,true);
            for(const auto &subelement : sub.subElements) {
                path->truncate(2);
                SoDetail *det = 0;
                if(!sub.linkInfo->getDetail(false,SnapshotTransform,subelement.c_str(),det,path))
                    continue;
                action.setElement(det);
                action.apply(path);
                delete det;
            }
        }
    }
    replaceLinkedRoot(linkedRoot);
}

bool LinkView::linkGetElementPicked(const SoPickedPoint *pp, std::string &subname) const 
{
    std::stringstream str;
    CoinPtr<SoPath> path = pp->getPath();
    if(nodeArray.size()) {
        auto idx = path->findNode(pcLinkRoot);
        if(idx<0 || idx+2>=path->getLength()) 
            return false;
        auto node = path->getNode(idx+1);
        auto it = nodeMap.find(node);
        if(it == nodeMap.end() || !isElementVisible(it->second))
            return false;
        str << it->second << '.';

        auto &info = *nodeArray[it->second];
        if(info.isLinked()) {
            if(!info.linkInfo->getElementPicked(false,childType,pp,str))
                return false;
            subname = str.str();
            return true;
        }
    }

    if(!isLinked()) return false;

    if(nodeType >= 0) {
        if(linkInfo->getElementPicked(false,nodeType,pp,str)) {
            subname = str.str();
            return true;
        }
        return false;
    }
    auto idx = path->findNode(pcLinkedRoot);
    if(idx<0 || idx+1>=path->getLength()) return false;
    auto node = path->getNode(idx+1);
    for(auto &v : subInfo) {
        auto &sub = *v.second;
        if(node != sub.pcNode) continue;
        std::stringstream str2;
        if(!sub.linkInfo->getElementPicked(false,SnapshotTransform,pp,str2))
            return false;
        const std::string &element = str2.str();
        if(sub.subElements.size()) {
            if(sub.subElements.find(element)==sub.subElements.end()) {
                auto pos = element.find('.');
                if(pos==std::string::npos ||
                   sub.subElements.find(element.c_str()+pos+1)==sub.subElements.end())
                    return false;
            }
        }
        if(!autoSubLink || subInfo.size()>1)
            str << v.first;
        str << element;
        subname = str.str();
        return true;
    }
    return false;
}

bool LinkView::linkGetDetailPath(const char *subname, SoFullPath *path, SoDetail *&det) const 
{
    if(!subname || *subname==0) return true;
    auto len = path->getLength();
    if(nodeArray.empty()) {
        appendPath(path,pcLinkRoot);
    }else{
        int idx = App::LinkBaseExtension::getArrayIndex(subname,&subname);
        if(idx<0 || idx>=(int)nodeArray.size()) 
            return false;

        auto &info = *nodeArray[idx];
        appendPath(path,pcLinkRoot);
        appendPath(path,info.pcSwitch);
        appendPath(path,info.pcRoot);

        if(*subname == 0) 
            return true;

        if(info.isLinked()) {
            info.linkInfo->getDetail(false,childType,subname,det,path);
            return true;
        }
    }
    if(isLinked()) {
        if(nodeType >= 0) {
            if(linkInfo->getDetail(false,nodeType,subname,det,path))
                return true;
        }else {
            appendPath(path,pcLinkedRoot);
            for(auto &v : subInfo) {
                auto &sub = *v.second;
                if(!sub.isLinked())
                    continue;
                const char *nextsub;
                if(autoSubLink && subInfo.size()==1)
                    nextsub = subname;
                else{
                    if(!boost::algorithm::starts_with(subname,v.first)) 
                        continue;
                    nextsub = subname+v.first.size();
                    if(*nextsub != '.') 
                        continue;
                    ++nextsub;
                }
                if(*nextsub && sub.subElements.size() && 
                   sub.subElements.find(nextsub)==sub.subElements.end())
                    break;
                appendPath(path,sub.pcNode);
                len = path->getLength();
                if(sub.linkInfo->getDetail(false,SnapshotTransform,nextsub,det,path))
                    return true;
                break;
            }
        }
    }
    path->truncate(len);
    return false;
}

void LinkView::unlink(LinkInfoPtr info) {
    if(!info) return;
    if(info == linkOwner) {
        linkOwner->remove(this);
        linkOwner.reset();
    }
    if(info != linkInfo)
        return;
    if(linkInfo) {
        linkInfo->remove(this);
        linkInfo.reset();
    }
    pcLinkRoot->resetContext();
    if(pcLinkedRoot) {
        if(nodeArray.empty())
            resetRoot();
        else {
            for(auto &info : nodeArray) {
                int idx;
                if(!info->isLinked() && 
                   (idx=info->pcRoot->findChild(pcLinkedRoot))>=0)
                    info->pcRoot->removeChild(idx);
            }
        }
        pcLinkedRoot.reset();
    }
    subInfo.clear();
    return;
}

QIcon LinkView::getLinkedIcon(QPixmap px) const {
    auto link = linkInfo;
    if(autoSubLink && subInfo.size()==1) 
        link = subInfo.begin()->second->linkInfo;
    if(!link || !link->isLinked())
        return QIcon();
    return link->getIcon(px);
}

bool LinkView::hasSubs() const {
    return isLinked() && subInfo.size();
}

///////////////////////////////////////////////////////////////////////////////////

PROPERTY_SOURCE(Gui::ViewProviderLink, Gui::ViewProviderDocumentObject)

static const char *_LinkIcon = "Link";
static const char *_LinkArrayIcon = "LinkArray";
static const char *_LinkGroupIcon = "LinkGroup";
static const char *_LinkElementIcon = "LinkElement";

ViewProviderLink::ViewProviderLink()
    :linkType(LinkTypeNone),hasSubName(false),hasSubElement(false),useCenterballDragger(true)
{
    sPixmap = _LinkIcon;

    ADD_PROPERTY_TYPE(Selectable, (true), " Link", App::Prop_None, 0);

    ADD_PROPERTY_TYPE(OverrideMaterial, (false), " Link", App::Prop_None, "Override linked object's material");

    ParameterGrp::handle hGrp = App::GetApplication().GetParameterGroupByPath(
            "User parameter:BaseApp/Preferences/View");
    App::Material mat(App::Material::DEFAULT);
    mat.diffuseColor.setPackedValue(hGrp->GetUnsigned("DefaultLinkColor",0x66FFFF00));
    ADD_PROPERTY_TYPE(ShapeMaterial, (mat), " Link", App::Prop_None, 0);
    ShapeMaterial.setStatus(App::Property::MaterialEdit, true);

    ADD_PROPERTY_TYPE(DrawStyle,((long int)0), " Link", App::Prop_None, "");
    static const char* DrawStyleEnums[]= {"None","Solid","Dashed","Dotted","Dashdot",NULL};
    DrawStyle.setEnums(DrawStyleEnums);

    int lwidth = hGrp->GetInt("DefaultShapeLineWidth",2);
    ADD_PROPERTY_TYPE(LineWidth,(lwidth), " Link", App::Prop_None, "");

    static App::PropertyFloatConstraint::Constraints sizeRange = {1.0,64.0,1.0};
    LineWidth.setConstraints(&sizeRange);

    ADD_PROPERTY_TYPE(PointSize,(lwidth), " Link", App::Prop_None, "");
    PointSize.setConstraints(&sizeRange);

    ADD_PROPERTY(MaterialList,());
    MaterialList.setStatus(App::Property::NoMaterialListEdit, true);

    ADD_PROPERTY(OverrideMaterialList,());

    DisplayMode.setStatus(App::Property::Status::Hidden, true);

    linkView = new LinkView;
}

ViewProviderLink::~ViewProviderLink()
{
    linkView->setInvalid();
}

bool ViewProviderLink::isSelectable() const {
    return !pcDragger && Selectable.getValue();
}

void ViewProviderLink::attach(App::DocumentObject *pcObj) {
    addDisplayMaskMode(linkView->getLinkRoot(),"Link");
    setDisplayMaskMode("Link");
    inherited::attach(pcObj);
    checkIcon();
    if(pcObj->isDerivedFrom(App::LinkElement::getClassTypeId()))
        hide();
    linkView->setOwner(this);
}

void ViewProviderLink::reattach(App::DocumentObject *) {
    linkView->setOwner(this);
}

std::vector<std::string> ViewProviderLink::getDisplayModes(void) const
{
    std::vector<std::string> StrList = inherited::getDisplayModes();
    StrList.push_back("Link");
    return StrList;
}

QIcon ViewProviderLink::getIcon() const {
    if(getObject()->getLinkedObject(false)!=getObject()) {
        QIcon icon = linkView->getLinkedIcon(getOverlayPixmap());
        if(!icon.isNull())
            return icon;
    }
    return Gui::BitmapFactory().pixmap(sPixmap);
}

QPixmap ViewProviderLink::getOverlayPixmap() const {
    if(hasSubElement)
        return BitmapFactory().pixmap("LinkSubElement");
    else if(hasSubName)
        return BitmapFactory().pixmap("LinkSubOverlay");
    else
        return BitmapFactory().pixmap("LinkOverlay");
}

void ViewProviderLink::onChanged(const App::Property* prop) {
    if(isRestoring()) {
        inherited::onChanged(prop);
        return;
    }
    if (prop == &OverrideMaterial) {
        if(!OverrideMaterial.getValue()) {
            linkView->setMaterial(-1,0);
            for(int i=0;i<linkView->getSize();++i)
                linkView->setMaterial(i,0);
        }else
            applyMaterial();
    }else if (prop == &ShapeMaterial) {
        if(OverrideMaterial.getValue())
            linkView->setMaterial(-1,&ShapeMaterial.getValue());
    }else if(prop == &MaterialList || prop == &OverrideMaterialList) {
        applyMaterial();
    }else if(prop==&DrawStyle || prop==&PointSize || prop==&LineWidth) {
        if(!DrawStyle.getValue())
            linkView->setDrawStyle(0);
        else
            linkView->setDrawStyle(DrawStyle.getValue(),LineWidth.getValue(),PointSize.getValue());
    }

    inherited::onChanged(prop);
}

bool ViewProviderLink::setLinkType(App::LinkBaseExtension *ext) {
    auto propLink = ext->getLinkedObjectProperty();
    if(!propLink) return false;
    LinkType type;
    if(hasSubName)
        type = LinkTypeSubs;
    else
        type = LinkTypeNormal;
    if(linkType != type)
        linkType = type;
    switch(type) {
    case LinkTypeSubs:
        linkView->setNodeType(ext->linkTransform()?LinkView::SnapshotContainer:
                LinkView::SnapshotContainerTransform);
        break;
    case LinkTypeNormal:
        linkView->setNodeType(ext->linkTransform()?LinkView::SnapshotVisible:
                LinkView::SnapshotTransform);
        break;
    default:
        break;
    }
    return true;
}

App::LinkBaseExtension *ViewProviderLink::getLinkExtension() {
    if(!pcObject || !pcObject->getNameInDocument())
        return 0;
    return pcObject->getExtensionByType<App::LinkBaseExtension>(true);
}

const App::LinkBaseExtension *ViewProviderLink::getLinkExtension() const{
    if(!pcObject || !pcObject->getNameInDocument())
        return 0;
    return const_cast<App::DocumentObject*>(pcObject)->getExtensionByType<App::LinkBaseExtension>(true);
}

void ViewProviderLink::updateData(const App::Property *prop) {
    if(!isRestoring() && !pcObject->isRestoring()) {
        auto ext = getLinkExtension();
        if(ext) updateDataPrivate(getLinkExtension(),prop);
    }
    return inherited::updateData(prop);
}

void ViewProviderLink::updateDataPrivate(App::LinkBaseExtension *ext, const App::Property *prop) {
    if(!prop) return;
    if(prop == &ext->_LinkRecomputed) {
        if(linkView->hasSubs())
            linkView->updateLink();
    }else if(prop==ext->getScaleProperty() || prop==ext->getScaleVectorProperty()) {
        const auto &v = ext->getScaleVector();
        pcTransform->scaleFactor.setValue(v.x,v.y,v.z);
        linkView->renderDoubleSide(v.x*v.y*v.z < 0);
    }else if(prop == ext->getPlacementProperty() || prop == ext->getLinkPlacementProperty()) {
        auto propLinkPlacement = ext->getLinkPlacementProperty();
        if(!propLinkPlacement || propLinkPlacement == prop) {
            const auto &v = pcTransform->scaleFactor.getValue();
            const auto &pla = static_cast<const App::PropertyPlacement*>(prop)->getValue();
            ViewProviderGeometryObject::updateTransform(pla, pcTransform);
            pcTransform->scaleFactor.setValue(v);
        }
    }else if(prop == ext->getLinkedObjectProperty() ||
             prop == ext->getSubElementsProperty()) 
    {
        if(!prop->testStatus(App::Property::User3)) {
            std::vector<std::string> subs;
            const char *subname = ext->getSubName();
            std::string sub;
            if(subname)
                sub = subname;
            const char *subElement = ext->getSubElement();
            if(subElement) {
                hasSubElement = true;
                subs.push_back(sub+subElement);
            }else
                hasSubElement = false;
            for(const auto &s : ext->getSubElementsValue()) {
                if(s.empty()) continue;
                hasSubElement = true;
                subs.push_back(sub+s);
            }

            if(subs.empty() && sub.size())
                subs.push_back(sub);

            hasSubName = !subs.empty();
            setLinkType(ext);

            auto obj = ext->getLinkedObjectValue();
            linkView->setLink(obj,subs);
            signalChangeIcon();
        }
    }else if(prop == ext->getLinkTransformProperty()) {
        setLinkType(ext);
    }else if(prop==ext->getElementCountProperty()) {
        if(!ext->getShowElementValue()) 
            linkView->setSize(ext->getElementCountValue());
        checkIcon(ext);
    }else if(prop == ext->getShowElementProperty()) {
        if(!ext->getShowElementValue()) {

            const auto &elements = ext->getElementListValue();
            // elements is about to be collapsed, preserve the materials
            if(elements.size()) {
                std::vector<App::Material> materials;
                boost::dynamic_bitset<> overrideMaterials;
                overrideMaterials.resize(elements.size(),false);
                bool overrideMaterial = false;
                bool hasMaterial = false;
                materials.reserve(elements.size());
                for(size_t i=0;i<elements.size();++i) {
                    auto element = dynamic_cast<App::LinkElement*>(elements[i]);
                    if(!element) continue;
                    auto vp = dynamic_cast<ViewProviderLink*>(
                            Application::Instance->getViewProvider(element));
                    if(!vp) continue;
                    overrideMaterial = overrideMaterial || vp->OverrideMaterial.getValue();
                    hasMaterial = overrideMaterial || hasMaterial 
                        || vp->ShapeMaterial.getValue()!=ShapeMaterial.getValue();
                    materials.push_back(vp->ShapeMaterial.getValue());
                    overrideMaterials[i] = vp->OverrideMaterial.getValue();
                }
                if(!overrideMaterial)
                    overrideMaterials.clear();
                OverrideMaterialList.setStatus(App::Property::User3,true);
                OverrideMaterialList.setValue(overrideMaterials);
                OverrideMaterialList.setStatus(App::Property::User3,false);
                if(!hasMaterial)
                    materials.clear();
                MaterialList.setStatus(App::Property::User3,true);
                MaterialList.setValue(materials);
                MaterialList.setStatus(App::Property::User3,false);
                
                linkView->setSize(ext->getElementCountValue());
                applyMaterial();
            }
        }
    }else if(prop==ext->getScaleListProperty() || prop==ext->getPlacementListProperty()) {
        if(!prop->testStatus(App::Property::User3) && 
            linkView->getSize() && 
            !ext->getShowElementValue()) 
        {
            auto propPlacements = ext->getPlacementListProperty();
            auto propScales = ext->getScaleListProperty();
            if(propPlacements && linkView->getSize()) {
                const auto &touched = 
                    prop==propScales?propScales->getTouchList():propPlacements->getTouchList();
                if(touched.empty()) {
                    for(int i=0;i<linkView->getSize();++i) {
                        Base::Matrix4D mat;
                        if(propPlacements->getSize()>i) 
                            mat = (*propPlacements)[i].toMatrix();
                        if(propScales->getSize()>i) {
                            Base::Matrix4D s;
                            s.scale((*propScales)[i]);
                            mat *= s;
                        }
                        linkView->setTransform(i,mat);
                    }
                }else{
                    for(int i : touched) {
                        if(i<0 || i>=linkView->getSize())
                            continue;
                        Base::Matrix4D mat;
                        if(propPlacements->getSize()>i) 
                            mat = (*propPlacements)[i].toMatrix();
                        if(propScales->getSize()>i) {
                            Base::Matrix4D s;
                            s.scale((*propScales)[i]);
                            mat *= s;
                        }
                        linkView->setTransform(i,mat);
                    }
                }
            }
        }
    }else if(prop == ext->getVisibilityListProperty()) {
        const auto &vis = ext->getVisibilityListValue();
        for(size_t i=0;i<(size_t)linkView->getSize();++i) {
            if(vis.size()>i)
                linkView->setElementVisible(i,vis[i]);
            else
                linkView->setElementVisible(i,true);
        }
    }else if(prop == ext->getElementListProperty()) {
        if(ext->getShowElementValue()) {
            const auto &elements = ext->getElementListValue();
            if(OverrideMaterialList.getSize() || MaterialList.getSize()) {
                int i=-1;
                for(auto obj : elements) {
                    ++i;
                    auto vp = dynamic_cast<ViewProviderLink*>(
                            Application::Instance->getViewProvider(obj));
                    if(!vp) continue;
                    if(OverrideMaterialList.getSize()>i)
                        vp->OverrideMaterial.setValue(OverrideMaterialList[i]);
                    if(MaterialList.getSize()>i)
                        vp->ShapeMaterial.setValue(MaterialList[i]);
                }
                OverrideMaterialList.setSize(0);
                MaterialList.setSize(0);
            }
            linkView->setChildren(elements, ext->getVisibilityListValue());
        }
        checkIcon(ext);
    }
}

void ViewProviderLink::checkIcon(const App::LinkBaseExtension *ext) {
    if(!ext) {
        ext = getLinkExtension();
        if(!ext) return;
    }
    const char *icon;
    auto element = dynamic_cast<App::LinkElement*>(getObject());
    if(element)
        icon = _LinkElementIcon;
    else if(!ext->getLinkedObjectProperty() && ext->getElementListProperty())
        icon = _LinkGroupIcon;
    else if(ext->getElementCountValue())
        icon = _LinkArrayIcon;
    else
        icon = _LinkIcon;
    if(icon!=sPixmap) {
        sPixmap = icon;
        signalChangeIcon();
    }
}

void ViewProviderLink::applyMaterial() {
    if(OverrideMaterial.getValue())
        linkView->setMaterial(-1,&ShapeMaterial.getValue());
    for(int i=0;i<linkView->getSize();++i) {
        if(MaterialList.getSize()>i && 
           OverrideMaterialList.getSize()>i && OverrideMaterialList[i])
            linkView->setMaterial(i,&MaterialList[i]);
    }
}

void ViewProviderLink::finishRestoring() {
    FC_TRACE("finish restoring");
    auto ext = getLinkExtension();
    if(!ext) return;
    linkView->setDrawStyle(DrawStyle.getValue(),LineWidth.getValue(),PointSize.getValue());
    updateDataPrivate(ext,ext->getLinkedObjectProperty());
    if(ext->getLinkPlacementProperty())
        updateDataPrivate(ext,ext->getLinkPlacementProperty());
    else
        updateDataPrivate(ext,ext->getPlacementProperty());
    updateDataPrivate(ext,ext->getElementCountProperty());
    if(ext->getPlacementListProperty())
        updateDataPrivate(ext,ext->getPlacementListProperty());
    else
        updateDataPrivate(ext,ext->getScaleListProperty());
    updateDataPrivate(ext,ext->getElementListProperty());
    applyMaterial();

    // TODO: notify the tree. This is ugly, any other way?
    getDocument()->signalChangedObject(*this,ext->_LinkRecomputed);
}

bool ViewProviderLink::hasElements(const App::LinkBaseExtension *ext) const {
    if(!ext) {
        ext = getLinkExtension();
        if(!ext) return false;
    }
    const auto &elements = ext->getElementListValue();
    return elements.size() && (int)elements.size()==ext->getElementCountValue();
}

bool ViewProviderLink::isGroup(const App::LinkBaseExtension *ext) const {
    if(!ext) {
        ext = getLinkExtension();
        if(!ext) return false;
    }
    return ext->getElementListProperty() && !ext->getLinkedObjectProperty();
}

ViewProvider *ViewProviderLink::getLinkedView(
        bool real,const App::LinkBaseExtension *ext) const 
{
    if(!ext) 
        ext = getLinkExtension();
    auto obj = ext&&real?ext->getTrueLinkedObject(true):
        getObject()->getLinkedObject(true);
    if(obj && obj!=getObject())
        return Application::Instance->getViewProvider(obj);
    return 0;
}

std::vector<App::DocumentObject*> ViewProviderLink::claimChildren(void) const {
    auto ext = getLinkExtension();
    if(ext && !ext->getShowElementValue() && ext->getElementCountValue()) {
        // in array mode without element objects, we'd better not show the
        // linked object's children to avoid inconsistent behavior on selection.
        // We claim the linked object instead
        std::vector<App::DocumentObject*> ret;
        if(ext) {
            auto obj = ext->getTrueLinkedObject(true);
            if(obj) ret.push_back(obj);
        }
        return ret;
    } else if(hasElements(ext) || isGroup(ext))
        return ext->getElementListValue();
    if(!hasSubName) {
        auto linked = getLinkedView(true);
        if(linked)
            return linked->claimChildren();
    }
    return std::vector<App::DocumentObject*>();
}

bool ViewProviderLink::canDragObject(App::DocumentObject* obj) const {
    auto ext = getLinkExtension();
    if(isGroup(ext))
        return true;
    if(hasElements(ext))
        return false;
    auto linked = getLinkedView(false,ext);
    if(linked)
        return linked->canDragObject(obj);
    return false;
}

bool ViewProviderLink::canDragObjects() const {
    auto ext = getLinkExtension();
    if(isGroup(ext))
        return true;
    if(hasElements(ext))
        return false;
    auto linked = getLinkedView(false,ext);
    if(linked)
        return linked->canDragObjects();
    return false;
}

void ViewProviderLink::dragObject(App::DocumentObject* obj) {
    auto ext = getLinkExtension();
    if(isGroup(ext)) {
        const auto &objs = ext->getElementListValue();
        for(size_t i=0;i<objs.size();++i) {
            if(obj==objs[i]) {
                ext->setLink(i,0);
                break;
            }
        }
        return;
    }
    if(hasElements(ext)) 
        return;
    auto linked = getLinkedView(false);
    if(linked)
        linked->dragObject(obj);
}

bool ViewProviderLink::canDropObjects() const {
    auto ext = getLinkExtension();
    if(isGroup(ext))
        return true;
    if(hasElements(ext))
        return false;
    if(hasSubElement)
        return true;
    else if(hasSubName)
        return false;
    auto linked = getLinkedView(false,ext);
    if(linked)
        return linked->canDropObjects();
    return true;
}

bool ViewProviderLink::canDropObjectEx(App::DocumentObject *obj, 
        App::DocumentObject *owner, const char *subname, const std::vector<std::string> &elements) const
{
    auto ext = getLinkExtension();
    if(isGroup(ext))
        return true;
    if(!ext || !ext->getLinkedObjectProperty() || hasElements(ext))
        return false;
    if(!hasSubName && linkView->isLinked()) {
        auto linked = getLinkedView(false,ext);
        if(linked)
            return linked->canDropObjectEx(obj,owner,subname,elements);
    }
    if(obj->getDocument() != getObject()->getDocument() && 
       !dynamic_cast<App::PropertyXLink*>(ext->getLinkedObjectValue()))
        return false;

    return true;
}

void ViewProviderLink::dropObjectEx(App::DocumentObject* obj, 
        App::DocumentObject *owner, const char *subname, const std::vector<std::string> &elements) 
{
    auto ext = getLinkExtension();
    if(isGroup(ext)) {
        ext->setLink(ext->getElementListValue().size(),obj);
        if(obj->getDocument()==getObject()->getDocument() && obj->Visibility.getValue())
            obj->Visibility.setValue(false);
        return;
    }

    if(!ext || !ext->getLinkedObjectProperty() || hasElements(ext))
        return;

    if(!hasSubName) {
        auto linked = getLinkedView(false,ext);
        if(linked) {
            linked->dropObjectEx(obj,owner,subname,elements);
            return;
        }
    }
    ext->setLink(-1,owner,subname);
}

bool ViewProviderLink::canDragAndDropObject(App::DocumentObject* obj) const {
    auto ext = getLinkExtension();
    if(!ext) return true;
    if(isGroup(ext)) {
        return ext->getLinkModeValue()<App::LinkBaseExtension::LinkModeAutoLink &&
               obj->getDocument()==getObject()->getDocument();
    }
    if(!ext->getLinkedObjectProperty() || hasElements(ext))
        return false;
    if(!hasSubName) {
        auto linked = getLinkedView(false,ext);
        if(linked) 
            return linked->canDragAndDropObject(obj);
    }
    return false;
}

bool ViewProviderLink::getElementPicked(const SoPickedPoint *pp, std::string &subname) const {
    if(!isSelectable()) return false;
    auto ext = getLinkExtension();
    if(!ext) return false;
    bool ret = linkView->linkGetElementPicked(pp,subname);
    if(ret && (isGroup(ext) || hasElements(ext))) {
        const auto &elements = ext->getElementListValue();
        const char *sub = 0;
        int idx = App::LinkBaseExtension::getArrayIndex(subname.c_str(),&sub);
        assert(idx>=0 && idx<(int)elements.size());
        assert(sub!=subname.c_str());
        --sub;
        assert(*sub == '.');
        subname.replace(0,sub-subname.c_str(),elements[idx]->getNameInDocument());
    }
    return ret;
}

bool ViewProviderLink::getDetailPath(
        const char *subname, SoFullPath *pPath, bool append, SoDetail *&det) const 
{
    auto ext = getLinkExtension();
    if(!ext) return false;

    auto len = pPath->getLength();
    if(append) {
        appendPath(pPath,pcRoot);
        appendPath(pPath,pcModeSwitch);
    }
    std::string _subname;
    if(subname && subname[0] && (isGroup(ext) || hasElements(ext))) {
        int index = ext->getElementIndex(subname,&subname);
        if(index>=0) {
            char idx[40];
            snprintf(idx,sizeof(idx),"%d.",index);
            _subname = idx;
            _subname += subname;
            subname = _subname.c_str();
        }
    }
    if(linkView->linkGetDetailPath(subname,pPath,det))
        return true;
    pPath->truncate(len);
    return false;
}

bool ViewProviderLink::onDelete(const std::vector<std::string> &) {
    auto element = dynamic_cast<App::LinkElement*>(getObject());
    return !element || element->canDelete();
}

bool ViewProviderLink::canDelete(App::DocumentObject *obj) const {
    auto ext = getLinkExtension();
    if(isGroup(ext) || hasElements(ext) || hasSubElement)
        return true;
    auto linked = getLinkedView(false,ext);
    if(linked)
        return linked->canDelete(obj);
    return false;
}

bool ViewProviderLink::linkEdit(const App::LinkBaseExtension *ext) const {
    if(!ext)
        ext = getLinkExtension();
    if(!ext ||
       (!ext->getShowElementValue() && ext->getElementCountValue()) ||
       hasElements(ext) || 
       isGroup(ext) ||
       hasSubName)
    {
        return false;
    }
    return linkView->isLinked();
}

bool ViewProviderLink::doubleClicked() {
    if(linkEdit())
        return linkView->getLinkedView()->doubleClicked();
    return getDocument()->setEdit(this,ViewProvider::Transform);
}

void ViewProviderLink::setupContextMenu(QMenu* menu, QObject* receiver, const char* member)
{
    if(linkEdit()) 
        linkView->getLinkedView()->setupContextMenu(menu,receiver,member);
}

bool ViewProviderLink::initDraggingPlacement() {
    Base::PyGILStateLocker lock;
    try {
        auto* proxy = getPropertyByName("Proxy");
        if (proxy && proxy->getTypeId() == App::PropertyPythonObject::getClassTypeId()) {
            Py::Object feature = static_cast<App::PropertyPythonObject*>(proxy)->getValue();
            const char *fname = "initDraggingPlacement";
            if (feature.hasAttr(fname)) {
                Py::Callable method(feature.getAttr(fname));
                Py::Tuple arg;
                Py::Object ret(method.apply(arg));
                if(!ret.isTrue())
                    return false;
                PyObject *pymat,*pypla,*pybbox;
                if(!PyArg_ParseTuple(ret.ptr(),"O!O!O!",&Base::MatrixPy::Type, &pymat,
                            &Base::PlacementPy::Type, &pypla,
                            &Base::BoundBoxPy::Type, &pybbox)) {
                    FC_ERR("initDraggingPlacement() expects return of type tuple(matrix,placement,boundbox)");
                    return false;
                }
                dragCtx.reset(new DraggerContext);
                dragCtx->initialPlacement = *static_cast<Base::PlacementPy*>(pypla)->getPlacementPtr();
                dragCtx->preTransform = *static_cast<Base::MatrixPy*>(pymat)->getMatrixPtr();
                dragCtx->bbox = *static_cast<Base::BoundBoxPy*>(pybbox)->getBoundBoxPtr();
                return true;
            }
        }
    } catch (Py::Exception&) {
        Base::PyException e;
        e.ReportException();
        return false;
    }

    auto ext = getLinkExtension();
    if(!ext) {
        FC_ERR("no link extension");
        return false;
    }
    if(!ext->hasPlacement()) {
        FC_ERR("no placement");
        return false;
    }
    auto doc = Application::Instance->editDocument();
    if(!doc) {
        FC_ERR("no editing document");
        return false;
    }

    dragCtx.reset(new DraggerContext);

    const auto &pla = ext->getPlacementProperty()?
        ext->getPlacementValue():ext->getLinkPlacementValue();

    dragCtx->preTransform = doc->getEditingTransform();
    auto plaMat = pla.toMatrix();
    plaMat.inverse();
    dragCtx->preTransform *= plaMat;

    dragCtx->bbox = linkView->getBoundBox();
    const auto &offset = Base::Placement(
            dragCtx->bbox.GetCenter(),Base::Rotation());
    dragCtx->initialPlacement = pla * offset;
    dragCtx->mat = offset.toMatrix();
    dragCtx->mat.inverse();
    return true;
}

ViewProvider *ViewProviderLink::startEditing(int mode) {
    if(mode==ViewProvider::Transform) {
        if(!initDraggingPlacement())
            return 0;
        if(useCenterballDragger)
            pcDragger = CoinPtr<SoCenterballDragger>(new SoCenterballDragger);
        else
            pcDragger = CoinPtr<SoFCCSysDragger>(new SoFCCSysDragger);
        updateDraggingPlacement(dragCtx->initialPlacement,true);
        pcDragger->addStartCallback(dragStartCallback, this);
        pcDragger->addFinishCallback(dragFinishCallback, this);
        pcDragger->addMotionCallback(dragMotionCallback, this);
        return inherited::startEditing(mode);
    }

    if(!linkEdit()) {
        FC_ERR("unsupported edit mode " << mode);
        return 0;
    }

    auto doc = Application::Instance->editDocument();
    if(!doc) {
        FC_ERR("no editing document");
        return 0;
    }

    // We are forwarding the editing request to linked object. We need to
    // adjust the editing transformation.
    Base::Matrix4D mat;
    auto linked = getObject()->getLinkedObject(true,&mat,false);
    if(!linked || linked==getObject()) {
        FC_ERR("no linked object");
        return 0;
    }
    auto vpd = dynamic_cast<ViewProviderDocumentObject*>(
                Application::Instance->getViewProvider(linked));
    if(!vpd) {
        FC_ERR("no linked viewprovider");
        return 0;
    }
    // amend the editing transformation with the link transformation
    doc->setEditingTransform(doc->getEditingTransform()*mat);
    return vpd->startEditing(mode);
}

void ViewProviderLink::setEditViewer(Gui::View3DInventorViewer* viewer, int ModNum)
{
    Q_UNUSED(ModNum);

    if (pcDragger && viewer)
    {
        SoPickStyle *rootPickStyle = new SoPickStyle();
        rootPickStyle->style = SoPickStyle::UNPICKABLE;
        static_cast<SoFCUnifiedSelection*>(
                viewer->getSceneGraph())->insertChild(rootPickStyle, 0);

        if(useCenterballDragger) {
            auto dragger = static_cast<SoCenterballDragger*>(pcDragger.get());
            SoSeparator *group = new SoAnnotation;
            SoPickStyle *pickStyle = new SoPickStyle;
            pickStyle->setOverride(true);
            group->addChild(pickStyle);
            group->addChild(pcDragger);

            // Because the dragger is not grouped with the actually geometry,
            // we use an invisible cube sized by the bound box obtained from
            // initDraggingPlacement() to scale the centerball dragger properly

            auto * ss = (SoSurroundScale*)dragger->getPart("surroundScale", TRUE);
            ss->numNodesUpToContainer = 3;
            ss->numNodesUpToReset = 2;

            auto *geoGroup = new SoGroup;
            group->addChild(geoGroup);
            auto *style = new SoDrawStyle;
            style->style.setValue(SoDrawStyle::INVISIBLE);
            style->setOverride(TRUE);
            geoGroup->addChild(style);
            auto *cube = new SoCube;
            geoGroup->addChild(cube);
            auto length = std::max(std::max(dragCtx->bbox.LengthX(),
                        dragCtx->bbox.LengthY()), dragCtx->bbox.LengthZ());
            cube->width = length;
            cube->height = length;
            cube->depth = length;

            viewer->setupEditingRoot(group,&dragCtx->preTransform);
        }else{
            SoFCCSysDragger* dragger = static_cast<SoFCCSysDragger*>(pcDragger.get());
            dragger->draggerSize.setValue(0.05f);
            dragger->setUpAutoScale(viewer->getSoRenderManager()->getCamera());
            viewer->setupEditingRoot(pcDragger,&dragCtx->preTransform);

            TaskCSysDragger *task = new TaskCSysDragger(this, dragger);
            Gui::Control().showDialog(task);
        }
    }
}

void ViewProviderLink::unsetEditViewer(Gui::View3DInventorViewer* viewer)
{
    SoNode *child = static_cast<SoFCUnifiedSelection*>(viewer->getSceneGraph())->getChild(0);
    if (child && child->isOfType(SoPickStyle::getClassTypeId()))
        static_cast<SoFCUnifiedSelection*>(viewer->getSceneGraph())->removeChild(child);
    pcDragger.reset();
    dragCtx.reset();
    Gui::Control().closeDialog();
}

Base::Placement ViewProviderLink::currentDraggingPlacement() const{
    assert(pcDragger);
    SbVec3f v;
    SbRotation r;
    if(useCenterballDragger) {
        SoCenterballDragger *dragger = static_cast<SoCenterballDragger*>(pcDragger.get());
        v = dragger->center.getValue();
        r = dragger->rotation.getValue();
    }else{
        SoFCCSysDragger *dragger = static_cast<SoFCCSysDragger*>(pcDragger.get());
        v = dragger->translation.getValue();
        r = dragger->rotation.getValue();
    }
    float q1,q2,q3,q4;
    r.getValue(q1,q2,q3,q4);
    return Base::Placement(Base::Vector3d(v[0],v[1],v[2]),Base::Rotation(q1,q2,q3,q4));
}

void ViewProviderLink::enableCenterballDragger(bool enable) {
    if(enable == useCenterballDragger)
        return;
    if(pcDragger)
        LINK_THROW(Base::RuntimeError,"Cannot change dragger during dragging");
    useCenterballDragger = enable;
}

void ViewProviderLink::updateDraggingPlacement(const Base::Placement &pla, bool force) {
    if(pcDragger && (force || currentDraggingPlacement()!=pla)) {
        const auto &pos = pla.getPosition();
        const auto &rot = pla.getRotation();
        FC_LOG("updating dragger placement (" << pos.x << ", " << pos.y << ", " << pos.z << ')');
        if(useCenterballDragger) {
            SoCenterballDragger *dragger = static_cast<SoCenterballDragger*>(pcDragger.get());
            SbBool wasenabled = dragger->enableValueChangedCallbacks(FALSE);
            SbMatrix matrix;
            matrix = convert(pla.toMatrix());
            dragger->center.setValue(SbVec3f(0,0,0));
            dragger->setMotionMatrix(matrix);
            if (wasenabled) {
                dragger->enableValueChangedCallbacks(TRUE);
                dragger->valueChanged();
            }
        }else{
            SoFCCSysDragger *dragger = static_cast<SoFCCSysDragger*>(pcDragger.get());
            dragger->translation.setValue(SbVec3f(pos.x,pos.y,pos.z));
            dragger->rotation.setValue(rot[0],rot[1],rot[2],rot[3]);
        }
    }
}

bool ViewProviderLink::callDraggerProxy(const char *fname, bool update) {
    if(!pcDragger) return false;
    Base::PyGILStateLocker lock;
    try {
        auto* proxy = getPropertyByName("Proxy");
        if (proxy && proxy->getTypeId() == App::PropertyPythonObject::getClassTypeId()) {
            Py::Object feature = static_cast<App::PropertyPythonObject*>(proxy)->getValue();
            if (feature.hasAttr(fname)) {
                Py::Callable method(feature.getAttr(fname));
                Py::Tuple args;
                method.apply(args);
                return true;
            }
        }
    } catch (Py::Exception&) {
        Base::PyException e;
        e.ReportException();
        return true;
    }

    if(update) {
        auto ext = getLinkExtension();
        if(ext) {
            const auto &pla = currentDraggingPlacement();
            auto prop = ext->getLinkPlacementProperty();
            if(!prop)
                prop = ext->getPlacementProperty();
            if(prop) {
                auto plaNew = pla * Base::Placement(dragCtx->mat);
                if(prop->getValue()!=plaNew)
                    prop->setValue(plaNew);
            }
            updateDraggingPlacement(pla);
        }
    }
    return false;
}

void ViewProviderLink::dragStartCallback(void *data, SoDragger *) {
    auto me = static_cast<ViewProviderLink*>(data);
    me->dragCtx->initialPlacement = me->currentDraggingPlacement();
    if(!me->callDraggerProxy("onDragStart",false)) {
        me->dragCtx->cmdPending = true;
        me->getDocument()->openCommand("Link Transform");
    }else
        me->dragCtx->cmdPending = false;
}

void ViewProviderLink::dragFinishCallback(void *data, SoDragger *) {
    auto me = static_cast<ViewProviderLink*>(data);
    me->callDraggerProxy("onDragEnd",true);
    if(me->dragCtx->cmdPending) {
        if(me->currentDraggingPlacement() == me->dragCtx->initialPlacement)
            me->getDocument()->abortCommand();
        else
            me->getDocument()->commitCommand();
    }
}

void ViewProviderLink::dragMotionCallback(void *data, SoDragger *) {
    auto me = static_cast<ViewProviderLink*>(data);
    me->callDraggerProxy("onDragMotion",true);
}

void ViewProviderLink::updateLinks(ViewProvider *vp) {
    auto ext = vp->getExtensionByType<ViewProviderLinkObserver>(true);
    if(ext && ext->linkInfo)
        ext->linkInfo->update();
}

PyObject *ViewProviderLink::getPyObject() {
    if (!pyViewObject)
        pyViewObject = new ViewProviderLinkPy(this);
    pyViewObject->IncRef();
    return pyViewObject;
}

PyObject *ViewProviderLink::getPyLinkView() {
    return linkView->getPyObject();
}


////////////////////////////////////////////////////////////////////////////////////////

namespace Gui {
PROPERTY_SOURCE_TEMPLATE(Gui::ViewProviderLinkPython, Gui::ViewProviderLink)
template class GuiExport ViewProviderPythonFeatureT<ViewProviderLink>;
}