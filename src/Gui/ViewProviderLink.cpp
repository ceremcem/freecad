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
# include <Inventor/nodes/SoTransform.h>
# include <Inventor/SoPickedPoint.h>
# include <Inventor/details/SoDetail.h>
# include <Inventor/misc/SoChildList.h>
# include <Inventor/nodes/SoMaterial.h>
#endif
#include <atomic>
#include <QApplication>
#include <QFileInfo>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/bind.hpp>
#include <Base/Console.h>
#include "Application.h"
#include "BitmapFactory.h"
#include "Document.h"
#include "Selection.h"
#include "MainWindow.h"
#include "ViewProviderLink.h"
#include "ViewProviderGeometryObject.h"
#include <App/GroupExtension.h>
#include <SoFCUnifiedSelection.h>

FC_LOG_LEVEL_INIT("App::Link",true,true)

using namespace Gui;

////////////////////////////////////////////////////////////////////////////

#ifdef FC_DEBUG
#define appendPath(_path,_node)  \
do{\
    if(_path->getLength()) {\
        SoNode * tail = _path->getTail();\
        const SoChildList * children = tail->getChildren();\
        if(!children || children->find((void *)_node)<0) {\
            FC_ERR("Link: coin path error");\
            throw Base::RuntimeError("Link: coin path error");\
        }\
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
    std::atomic<int> vref; //visibility ref counter

    boost::signals::connection connChangeIcon;

    ViewProviderDocumentObject *pcLinked;
    std::multiset<LinkHandle*> links;

    typedef LinkInfoPtr Pointer;

    std::array<CoinPtr<SoSeparator>,LinkHandle::SnapshotMax> pcSnapshots;
    std::array<CoinPtr<SoSwitch>,LinkHandle::SnapshotMax> pcSwitches;
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

    static Pointer get(App::DocumentObject *obj, LinkHandle *l=0) {
        return get(getView(obj),l);
    }

    static Pointer get(ViewProviderDocumentObject *vp, LinkHandle *l=0) {
        if(!vp) return Pointer();
        (void)l;

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
        return ext->linkInfo;
    }

    LinkInfo(ViewProviderDocumentObject *vp)
        :ref(0),vref(0),pcLinked(vp) 
    {
        FC_LOG("new link to " << pcLinked->getObject()->getNameInDocument());
        connChangeIcon = vp->signalChangeIcon.connect(
                boost::bind(&LinkInfo::slotChangeIcon,this));
    }

    ~LinkInfo() {
    }

    void setVisible(bool visible) {
        if(visible) {
            if(++vref == 1) {
                if(pcLinked) {
                    pcLinked->forceUpdate();
                    if(!pcLinked->isShow())
                        pcLinked->updateView();
                }
            }
        }else if(vref>0) {
            if(--vref == 0 && pcLinked)
                pcLinked->forceUpdate(false);
        }else
            FC_WARN("visibility ref count error");
    }

    void getNodeNames(Document *pDocument, QMap<SoNode*, QString> &nodeNames) const {
        if(!isLinked()) return;
        QString label;
        if(pDocument != pcLinked->getDocument())
            label = QString::fromUtf8(getDocName());
        label += QString::fromAscii("->") + 
            QString::fromUtf8(pcLinked->getObject()->Label.getValue());
        for(auto node : pcSnapshots)
            nodeNames[node] = label;
        for(auto v : nodeMap) 
            v.second->getNodeNames(pDocument,nodeNames);
    }

    bool checkName(const char *name) const {
        return isLinked() && strcmp(name,getLinkedName())==0;
    }

    void remove(LinkHandle *l) {
        auto it = links.find(l);
        if(it!=links.end()) {
            if(l->linkInfo!=this || l->getVisibility()) 
                setVisible(false);
            links.erase(it);
        }
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
        std::multiset<LinkHandle*> linksTmp;
        linksTmp.swap(links);
        auto me = LinkInfoPtr(this);
        for(auto link : linksTmp)
            link->unlink(me);
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
        if(vref && pcLinked)
            pcLinked->forceUpdate(false);
        pcLinked = 0;
        vref = 0;
        connChangeIcon.disconnect();
    }

    void updateSwitch() {
        if(!isLinked() || !pcLinkedSwitch) return;
        int index = pcLinkedSwitch->whichChild.getValue();
        for(size_t i=0;i<pcSwitches.size();++i) {
            if(!pcSwitches[i]) 
                continue;
            int count = pcSwitches[i]->getNumChildren();
            if((index<0 && i==LinkHandle::SnapshotChild) || !count)
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
                if(ext) ext->extensionBeforeDelete();
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
        if(type<0 || type>=LinkHandle::SnapshotMax)
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

        for(int i=0,count=root->getNumChildren();i<count;++i) {
            SoNode *node = root->getChild(i);
            if(type==LinkHandle::SnapshotTransform && 
               node->isOfType(SoTransform::getClassTypeId()))
                continue;
            if(!node->isOfType(SoSwitch::getClassTypeId())) {
                pcSnapshot->addChild(node);
                continue;
            }
            if(pcLinkedSwitch) {
                FC_WARN(getLinkedName() << " more than one switch node");
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
                Pointer info = get(child);
                if(!info) continue;
                SoNode *node = info->getSnapshot(LinkHandle::SnapshotChild);
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
        if(!pp || !isLinked())
            return false;

        if(addname) 
            str << getLinkedName() <<'.';
        
        auto pcSwitch = pcSwitches[type];
        if(pcChildGroup && pcSwitch && 
            pcSwitch->getChild(pcSwitch->whichChild.getValue())==pcChildGroup)
        {
            SoPath *path = pp->getPath();
            int index = path->findNode(pcChildGroup);
            if(index<=0) return false;
            auto it = nodeMap.find(path->getNode(index+1));
            if(it==nodeMap.end()) return false;
            return it->second->getElementPicked(true,LinkHandle::SnapshotChild,pp,str);
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

        if(path) {
            appendPath(path,pcSnapshots[type]);
            appendPath(path, pcSwitches[type]);
        }
        if(*subname == 0) return true;

        auto pcSwitch = pcSwitches[type];
        if(!pcChildGroup || !pcSwitch ||
            pcSwitch->getChild(pcSwitch->whichChild.getValue())!=pcChildGroup)
        {
            det = pcLinked->getDetailPath(subname,path,false);
            return true;
        }
        if(path){
            appendPath(path,pcChildGroup);
            if(pcLinked->getChildRoot())
                type = LinkHandle::SnapshotChild;
            else
                type = LinkHandle::SnapshotVisible;
        }
        for(auto v : nodeMap) {
            if(v.second->getDetail(true,type,subname,det,path))
                return true;
        }
        return false;
    }

    void slotChangeIcon() {
        iconMap.clear();
        if(!isLinked()) 
            return;
        auto me = LinkInfoPtr(this);
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

void ViewProviderLinkObserver::extensionUpdateData(const App::Property *) {
    if(linkInfo) linkInfo->update();
}

void ViewProviderLinkObserver::extensionFinishRestoring() {
    if(linkInfo) {
        FC_TRACE("linked finish restoing");
        linkInfo->update();
    }
}

///////////////////////////////////////////////////////////////////////////////////

TYPESYSTEM_SOURCE(Gui::LinkHandle,Base::BaseClass);

LinkHandle::LinkHandle()
    :owner(0),nodeType(SnapshotTransform),autoSubLink(true),visible(false)
{
    pcLinkRoot = new SoFCSelectionRoot;
    pcMaterial = new SoMaterial;
    pcLinkRoot->addChild(pcMaterial);
}

LinkHandle::~LinkHandle() {
    unlink(LinkInfoPtr());
}

void LinkHandle::setMaterial(int index, const App::Material *material) {
    auto pcMat = pcMaterial;
    if(index < 0) {
        if(!material) {
            pcMaterial->setOverride(false);
            return;
        }
    }else if(index >= (int)nodeArray.size())
        throw Base::ValueError("Link: material index out of range");
    else if(nodeArray[index].pcMaterial == pcMaterial) {
        if(!material) 
            return;
        nodeArray[index].pcMaterial = pcMat = new SoMaterial;
        nodeArray[index].pcRoot->replaceChild(pcMaterial,pcMat);
    }else if (!material) {
        nodeArray[index].pcRoot->replaceChild(nodeArray[index].pcMaterial,pcMaterial);
        nodeArray[index].pcMaterial = pcMaterial;
        return;
    }else
        pcMat = nodeArray[index].pcMaterial;

    pcMat->setOverride(true);

    const App::Material &Mat = *material;
    pcMat->ambientColor.setValue(Mat.ambientColor.r,Mat.ambientColor.g,Mat.ambientColor.b);
    pcMat->specularColor.setValue(Mat.specularColor.r,Mat.specularColor.g,Mat.specularColor.b);
    pcMat->emissiveColor.setValue(Mat.emissiveColor.r,Mat.emissiveColor.g,Mat.emissiveColor.b);
    pcMat->shininess.setValue(Mat.shininess);
    pcMat->diffuseColor.setValue(Mat.diffuseColor.r,Mat.diffuseColor.g,Mat.diffuseColor.b);
    pcMat->transparency.setValue(Mat.transparency);
}

void LinkHandle::setLink(App::DocumentObject *obj, const std::vector<std::string> &subs) {
    bool reorder = false;
    if(!isLinked() || linkInfo->pcLinked->getObject()!=obj) {
        unlink(LinkInfoPtr());

        linkInfo = LinkInfo::get(obj,this);
        if(!linkInfo) 
            return;

        linkInfo->links.insert(this);
        if(getVisibility())
            linkInfo->setVisible(true);

        reorder = true;
    }
    subInfo.clear();
    for(const auto &sub : subs) {
        if(sub.empty()) continue;
        const char *subelement = strrchr(sub.c_str(),'.');
        std::string subname;
        if(!subelement)
            subelement = sub.c_str();
        else {
            ++subelement;
            subname = sub.substr(0,subelement-sub.c_str());
        }
        auto it = subInfo.find(subname);
        if(it == subInfo.end())
            it = subInfo.insert(std::make_pair(subname,SubInfo(*this))).first;
        if(subelement[0])
            it->second.subElements.insert(subelement);
    }
    if(reorder && owner && subInfo.size())
        owner->getDocument()->reorderViewProviders(owner,linkInfo->pcLinked);
    onLinkUpdate();
}

void LinkHandle::setTransform(SoTransform *pcTransform, const Base::Matrix4D &mat) {

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
}

void LinkHandle::setSize(int _size) {
    size_t size;
    if(_size<0) 
        size = 0;
    else
        size = (size_t)_size;
    if(size == nodeArray.size()) return;
    if(!size) {
        nodeArray.clear();
        nodeMap.clear();
        pcLinkRoot->removeAllChildren();
        pcLinkRoot->addChild(pcMaterial);
        if(pcLinkedRoot)
            pcLinkRoot->addChild(pcLinkedRoot);
        return;
    }
    if(size<nodeArray.size()) {
        for(auto i=size;i<nodeArray.size();++i) {
            pcLinkRoot->removeChild(pcLinkRoot->getNumChildren()-1);
            auto it = nodeMap.find(nodeArray[i].pcSwitch);
            nodeMap.erase(it);
        }
        nodeArray.resize(size);
        return;
    }
    if(nodeArray.empty()) 
        pcLinkRoot->removeAllChildren();
    while(nodeArray.size()<size) {
        nodeArray.emplace_back();
        auto &info = nodeArray.back();
        info.pcSwitch = new SoSwitch;
        info.pcRoot = new SoFCSelectionRoot;
        info.pcMaterial = pcMaterial;
        info.pcTransform = new SoTransform;
        info.pcRoot->addChild(info.pcMaterial);
        info.pcRoot->addChild(info.pcTransform);
        if(pcLinkedRoot)
            info.pcRoot->addChild(pcLinkedRoot);
        info.pcSwitch->addChild(info.pcRoot);
        info.pcSwitch->whichChild = 0;
        pcLinkRoot->addChild(info.pcSwitch);
        nodeMap.insert(std::make_pair(info.pcSwitch,(int)nodeArray.size()-1));
    }
}

void LinkHandle::setTransform(int index, const Base::Matrix4D &mat) {
    if(index<0 || index>=(int)nodeArray.size())
        throw Base::ValueError("Link: index out of range");
    setTransform(nodeArray[index].pcTransform,mat);
}

int LinkHandle::setElementVisible(int idx, bool visible) {
    if(idx<0 || idx>=(int)nodeArray.size())
        return 0;
    nodeArray[idx].pcSwitch->whichChild = visible?0:-1;
    return 1;
}

bool LinkHandle::isElementVisible(int idx) const {
    if(idx<0 || idx>=(int)nodeArray.size())
        return false;
    return nodeArray[idx].pcSwitch->whichChild.getValue() == 0;
}

void LinkHandle::setNodeType(SnapshotType type, bool sublink) {
    autoSubLink = sublink;
    if(nodeType==type) return;
    if(type>=SnapshotMax || 
       (type<0 && type!=SnapshotContainer && type!=SnapshotContainerTransform))
        throw Base::ValueError("Link: invalid node type");

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
    onLinkUpdate();
}

void LinkHandle::replaceLinkedRoot(SoSeparator *root) {
    if(root==pcLinkedRoot) return;

    if(nodeArray.empty()) {
        if(pcLinkedRoot && root) 
            pcLinkRoot->replaceChild(pcLinkedRoot,root);
        else if(root)
            pcLinkRoot->addChild(root);
        else
            pcLinkRoot->removeChild(pcLinkedRoot);
    }else if(pcLinkedRoot && root) {
        for(auto &info : nodeArray)
            info.pcRoot->replaceChild(pcLinkedRoot,root);
    }else if(root) {
        for(auto &info : nodeArray)
            info.pcRoot->addChild(root);
    }else{
        for(auto &info : nodeArray)
            info.pcRoot->removeChild(pcLinkedRoot);
    }
    pcLinkedRoot = root;
}

void LinkHandle::onLinkedIconChange(LinkInfoPtr link) {
    if(!owner || !owner->getObject() || !owner->getObject()->getNameInDocument())
        return;

    if(link==linkInfo || 
        (subInfo.size() && subInfo.begin()->second.link==link))
    {
        if(link) 
            FC_LOG(owner->getObject()->getNameInDocument() << 
                " linked icon change " << link->getLinkedNameSafe());
        owner->signalChangeIcon();
    }
}

LinkHandle::SubInfo::SubInfo(LinkHandle &handle)
    :handle(handle)
{}

LinkHandle::SubInfo::~SubInfo() {
    unlink(true);
}

void LinkHandle::SubInfo::unlink(bool reset) {
    if(link) {
        link->remove(&handle);
        link.reset();
    }
    if(reset) {
        if(pcNode){
            auto root = handle.getLinkRoot();
            if(root) {
                int idx = root->findChild(pcNode);
                if(idx>=0)
                    root->removeChild(idx);
            }
            pcNode.reset();
        }
        pcTransform.reset();
    }else if(pcNode) {
        pcNode->removeAllChildren();
        pcNode->addChild(pcTransform);
    }
}

bool LinkHandle::SubInfo::isLinked() const {
    return link && link->isLinked();
}

void LinkHandle::onLinkUpdate() {
    if(!isLinked())
        return;

    if(owner && owner->isRestoring()) {
        FC_TRACE("restoring '" << owner->getObject()->getNameInDocument() << "'");
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
        auto &sub = v.second;
        Base::Matrix4D mat;
        App::DocumentObject *sobj = obj->getSubObject(
                v.first.c_str(), 0, &mat, nodeType==SnapshotContainer);
        if(!sobj) {
            sub.unlink();
            continue;
        }
        if(!sub.isLinked() || sub.link->pcLinked->getObject()!=sobj) {
            sub.unlink();
            sub.link = LinkInfo::get(sobj,this);
            if(!sub.link) continue;
            sub.link->links.insert(this);
            sub.link->setVisible(true);
            if(!sub.pcNode) {
                sub.pcNode = new SoFCSelectionRoot;
                sub.pcTransform = new SoTransform;
                sub.pcNode->addChild(sub.pcTransform);
            }
            sub.pcNode->addChild(sub.link->getSnapshot(SnapshotTransform));
        }
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
                if(!sub.link->getDetail(false,SnapshotTransform,subelement.c_str(),det,path))
                    continue;
                action.setElement(det);
                action.apply(path);
                delete det;
            }
        }
    }
    replaceLinkedRoot(linkedRoot);
}

void LinkHandle::setVisibility(bool visible) {
    if(this->visible != visible) {
        this->visible = visible;
        if(linkInfo) 
            linkInfo->setVisible(visible);
    }
}

bool LinkHandle::linkGetElementPicked(const SoPickedPoint *pp, std::string &subname) const 
{
    if(!isLinked()) return false;

    std::stringstream str;
    CoinPtr<SoPath> path = pp->getPath();
    if(nodeArray.size()) {
        auto idx = path->findNode(pcLinkRoot);
        if(idx<0 || idx+2>=path->getLength()) 
            return false;
        auto node = path->getNode(idx+1);
        auto it = nodeMap.find(node);
        if(it == nodeMap.end())
            return false;
        str << it->second << '.';
    }

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
        auto &sub = v.second;
        if(node != sub.pcNode) continue;
        std::stringstream str2;
        if(!sub.link->getElementPicked(false,SnapshotTransform,pp,str2))
            return false;
        const std::string &element = str2.str();
        if(sub.subElements.size() && element.size() && 
           sub.subElements.find(element)==sub.subElements.end())
            return false;
        if(!autoSubLink || subInfo.size()>1)
            str << v.first;
        str << element;
        subname = str.str();
        return true;
    }
    return false;
}

bool LinkHandle::isLinked() const {
    return linkInfo && linkInfo->isLinked();
}

ViewProviderDocumentObject *LinkHandle::linkGetLinkedView(bool recursive, int depth) const{
    auto link = linkInfo;
    if(autoSubLink && subInfo.size()==1)
        link = subInfo.begin()->second.link;
    if(link && link->isLinked()) {
        if(!recursive) return link->pcLinked;
        if(!App::GetApplication().checkLinkDepth(depth))
            return 0;
        return link->pcLinked->getLinkedView(true,depth+1);
    }
    return 0;
}

bool LinkHandle::linkGetDetailPath(const char *subname, SoFullPath *path, SoDetail *&det) const 
{
    if(!isLinked()) return false;
    if(!subname || *subname==0) return true;
    auto len = path->getLength();
    if(nodeArray.empty())
        appendPath(path,pcLinkRoot);
    else{
        int idx = App::LinkBaseExtension::getArrayIndex(subname,&subname);
        if(idx<0 || idx>=(int)nodeArray.size()) 
            return false;

        appendPath(path,pcLinkRoot);
        appendPath(path,nodeArray[idx].pcSwitch);
        appendPath(path,nodeArray[idx].pcRoot);

        if(*subname == 0) 
            return true;
    }
    if(nodeType >= 0) {
        if(linkInfo->getDetail(false,nodeType,subname,det,path))
            return true;
    }else {
        appendPath(path,pcLinkedRoot);
        for(auto &v : subInfo) {
            auto &sub = v.second;
            if(!sub.isLinked() || !sub.pcNode)
                continue;
            const char *nextsub;
            if(autoSubLink && subInfo.size()==1)
                nextsub = subname;
            else{
                if(!boost::algorithm::starts_with(subname,v.first)) 
                    continue;
                const char *nextsub = subname+v.first.size();
                if(*nextsub != '.') 
                    continue;
                ++nextsub;
            }
            if(*nextsub && 
               sub.subElements.size() && 
               sub.subElements.find(nextsub)==sub.subElements.end())
            {
                break;
            }
            appendPath(path,sub.pcNode);
            len = path->getLength();
            if(sub.link->getDetail(false,SnapshotTransform,nextsub,det,path))
                return true;
            break;
        }
    }
    path->truncate(len);
    return false;
}

void LinkHandle::unlink(LinkInfoPtr link) {
    if(!link || link==linkInfo) {
        if(linkInfo) {
            linkInfo->remove(this);
            linkInfo.reset();
        }
        pcLinkRoot->resetContext();
        if(pcLinkedRoot) {
            if(nodeArray.empty())
                pcLinkRoot->removeChild(pcLinkedRoot);
            else {
                for(auto &info : nodeArray)
                    info.pcRoot->removeChild(pcLinkedRoot);
            }
            pcLinkedRoot.reset();
        }
        subInfo.clear();
        return;
    }
    if(link) {
        for(auto &v : subInfo) {
            auto &sub = v.second;
            if(sub.link == link) {
                sub.unlink();
                break;
            }
        }
    }
}

QIcon LinkHandle::getLinkedIcon(QPixmap px) const {
    auto link = linkInfo;
    if(autoSubLink && subInfo.size()==1) 
        link = subInfo.begin()->second.link;
    if(!link || !link->isLinked())
        return QIcon();
    return link->getIcon(px);
}

bool LinkHandle::hasSubs() const {
    return isLinked() && subInfo.size();
}

///////////////////////////////////////////////////////////////////////////////////

PROPERTY_SOURCE(Gui::ViewProviderLink, Gui::ViewProviderDocumentObject)

ViewProviderLink::ViewProviderLink()
    :prevLinked(this),linkType(LinkTypeNone),linkTransform(false)
{
    ADD_PROPERTY_TYPE(Selectable, (true), " Link", App::Prop_None, 0);

    ADD_PROPERTY_TYPE(LinkUseMaterial, (false), " Link", App::Prop_None, "Override linked object's material");
    ADD_PROPERTY_TYPE(LinkShapeMaterial, (App::Material(App::Material::DEFAULT)), " Link", App::Prop_None, 0);
    LinkShapeMaterial.setStatus(App::Property::MaterialEdit, true);

    ADD_PROPERTY(MaterialList,());
    MaterialList.setStatus(App::Property::NoMaterialListEdit, true);

    ADD_PROPERTY(UseMaterialList,());

    DisplayMode.setStatus(App::Property::Status::Hidden, true);

    handle.setOwner(this);

    signalChangeIcon.connect(boost::bind(&ViewProviderLink::onChangeIcon,this));
}

ViewProviderLink::~ViewProviderLink()
{
}

void ViewProviderLink::attach(App::DocumentObject *pcObj) {
    addDisplayMaskMode(handle.getLinkRoot(),"Link");
    setDisplayMaskMode("Link");
    inherited::attach(pcObj);
}

std::vector<std::string> ViewProviderLink::getDisplayModes(void) const
{
    std::vector<std::string> StrList = inherited::getDisplayModes();
    StrList.push_back("Link");
    return StrList;
}

void ViewProviderLink::onChangeIcon() const {
    auto name = getObject()?getObject()->getNameInDocument():0;
    if(!name) name = "?";
    FC_LOG(name << " change icon");
    auto ext = getLinkExtension();
    if(hasElements(ext)) {
        for(auto obj : ext->getElementListValue()) {
            auto vp = Application::Instance->getViewProvider(obj);
            if(vp) vp->signalChangeIcon();
        }
    }
}

QIcon ViewProviderLink::getLinkedIcon() const {
    return handle.getLinkedIcon(getOverlayPixmap());
}

QIcon ViewProviderLink::getIcon() const {
    QIcon icon;
    if(hasElements()) 
        return Gui::BitmapFactory().pixmap("links");
    else if((icon=getLinkedIcon()).isNull())
        return Gui::BitmapFactory().pixmap("link");
    return icon;
}

bool ViewProviderLink::hasSubName(const App::LinkBaseExtension *ext) const {
    if(!ext) {
        ext = getLinkExtension();
        if(!ext) return 0;
    }
    auto xlink = dynamic_cast<const App::PropertyXLink*>(ext->getLinkedObjectProperty());
    return xlink && xlink->getSubName()[0]!=0;
}

QPixmap ViewProviderLink::getOverlayPixmap() const {
    if(hasSubName())
        return BitmapFactory().pixmap("linkSubOverlay");
    else
        return BitmapFactory().pixmap("linkOverlay");
}

void ViewProviderLink::onChanged(const App::Property* prop) {
    if(isRestoring()) {
        inherited::onChanged(prop);
        return;
    }
    if(prop == &Visibility) 
        handle.setVisibility(Visibility.getValue());
    else if (prop == &LinkUseMaterial) {
        if(!LinkUseMaterial.getValue()) {
            handle.setMaterial(-1,0);
            for(int i=0;i<handle.getSize();++i)
                handle.setMaterial(i,0);
        }else
            applyMaterial();
    }else if (prop == &LinkShapeMaterial) {
        if(!LinkUseMaterial.getValue())
            LinkUseMaterial.setValue(true);
        else
            handle.setMaterial(-1,&LinkShapeMaterial.getValue());
    }else if(prop == &MaterialList) {
        auto ext = getLinkExtension();
        if(!MaterialList.testStatus(App::Property::User3) && hasElements(ext)) {
            auto propElements = ext->getElementListProperty();
            for(int i=0;i<propElements->getSize();++i) {
                auto obj = (*propElements)[i];
                auto vp = Application::Instance->getViewProvider(obj);
                if(!vp || !vp->isDerivedFrom(ViewProviderLinkElement::getClassTypeId()))
                    continue;
                auto vpe = static_cast<ViewProviderLinkElement*>(vp);
                vpe->ShapeMaterial.setStatus(App::Property::User3,true);
                if(MaterialList.getSize()>i)
                    vpe->ShapeMaterial.setValue(MaterialList[i]);
                else
                    vpe->ShapeMaterial.setValue(LinkShapeMaterial.getValue());
                vpe->ShapeMaterial.setStatus(App::Property::User3,false);
            }
        }
        applyMaterial();
    }else if(prop == &UseMaterialList) {
        auto ext = getLinkExtension();
        if(!UseMaterialList.testStatus(App::Property::User3) && hasElements(ext)) {
            auto propElements = ext->getElementListProperty();
            for(int i=0;i<propElements->getSize();++i) {
                auto obj = (*propElements)[i];
                auto vp = Application::Instance->getViewProvider(obj);
                if(!vp || !vp->isDerivedFrom(ViewProviderLinkElement::getClassTypeId()))
                    continue;
                auto vpe = static_cast<ViewProviderLinkElement*>(vp);
                vpe->UseMaterial.setStatus(App::Property::User3,true);
                if(UseMaterialList.getSize()>i)
                    vpe->UseMaterial.setValue(UseMaterialList[i]);
                else
                    vpe->UseMaterial.setValue(false);
                vpe->UseMaterial.setStatus(App::Property::User3,false);
            }
        }
        applyMaterial();
    }

    inherited::onChanged(prop);
}

bool ViewProviderLink::setLinkType(App::LinkBaseExtension *ext) {
    auto propLink = ext->getLinkedObjectProperty();
    if(!propLink) return false;
    LinkType type;
    bool hasSub = hasSubName(ext);
    if(!hasSub) {
        for(const auto &subelement : ext->getSubElementsValue())
            if((hasSub = !subelement.empty()))
                break;
    }
    if(hasSub)
        type = LinkTypeSubs;
    else
        type = LinkTypeNormal;
    if(linkType != type)
        linkType = type;
    switch(type) {
    case LinkTypeSubs:
        handle.setNodeType(linkTransform?LinkHandle::SnapshotContainer:
                LinkHandle::SnapshotContainerTransform);
        break;
    case LinkTypeNormal:
        handle.setNodeType(linkTransform?LinkHandle::SnapshotVisible:
                LinkHandle::SnapshotTransform);
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

void ViewProviderLink::updateElementChildren(App::LinkBaseExtension *ext) {
    if(hasElements(ext)) {
        const auto &children = claimChildrenPrivate();
        if(children!=childrenCache) {
            childrenCache = children;
            // for notifying tree view of children change
            for(auto obj : ext->getElementListValue()) {
                if(obj && obj->getNameInDocument()) {
                    auto element = dynamic_cast<App::LinkElement*>(obj);
                    if(element)
                        obj->getDocument()->signalChangedObject(*obj,element->_Recomputed);
                }
            }
        }
    }
}

void ViewProviderLink::updateDataPrivate(App::LinkBaseExtension *ext, const App::Property *prop) {
    if(!prop) return;
    if(prop == &ext->_LinkRecomputed) {
        if(handle.hasSubs())
            handle.onLinkUpdate();
        auto linked = getLinkedView(false);
        if(linked!=prevLinked) {
            prevLinked = linked;
            signalChangeIcon();
        }
        updateElementChildren(ext);
    }else if(prop == ext->getScaleProperty()) {
        const auto &v = ext->getScaleValue();
        pcTransform->scaleFactor.setValue(v.x,v.y,v.z);
    }else if(prop == ext->getPlacementProperty() || prop == ext->getLinkPlacementProperty()) {
        auto propLinkPlacement = ext->getLinkPlacementProperty();
        if(!propLinkPlacement || propLinkPlacement == prop) {
            const auto &v = pcTransform->scaleFactor.getValue();
            ViewProviderGeometryObject::updateTransform(ext->getPlacementValue(), pcTransform);
            pcTransform->scaleFactor.setValue(v);
        }
    }else if(prop == ext->getLinkedObjectProperty() ||
             prop == ext->getSubElementsProperty()) 
    {
        if(!prop->testStatus(App::Property::User3)) {
            setLinkType(ext);
            std::vector<std::string> subs;
            auto xlink = dynamic_cast<const App::PropertyXLink*>(ext->getLinkedObjectProperty());
            const char *subname = xlink?xlink->getSubName():0;
            const auto &subElements = ext->getSubElementsValue();
            for(const auto &s : subElements) {
                if(s.size())
                    subs.push_back(s);
            }
            if(subname && *subname) {
                std::string sub(subname);
                if(sub[sub.size()-1]!='.')
                    sub += '.';
                if(subs.empty())
                    subs.push_back(sub);
                else{
                    for(auto &s : subs)
                        s = sub+s;
                }
            }

            auto obj = getObject()->getLinkedObject(false);
            handle.setLink(obj==getObject()?0:obj,subs);
            if(Visibility.getValue())
                handle.setVisibility(true);

            auto linked = getLinkedView(false);
            if(linked!=prevLinked) {
                prevLinked = linked;
                signalChangeIcon();
            }

            if(prop!=ext->getSubElementsProperty() && hasElements(ext)) {
                for(auto obj : ext->getElementListValue()) {
                    App::LinkElement *element;
                    if(obj && obj->getNameInDocument() && (element=dynamic_cast<App::LinkElement*>(obj)))
                        element->_Recomputed.touch();
                }
            }
        }
    }else if(prop == ext->getLinkTransformProperty()) {
        if(linkTransform != ext->getLinkTransformValue()) {
            linkTransform = !linkTransform;
            setLinkType(ext);
        }
    }else if(prop == ext->getElementCountProperty()) {
        auto propCount = ext->getElementCountProperty();
        auto propPlacements = ext->getPlacementListProperty();
        auto propScales = ext->getScaleListProperty();
        auto propElementVis = ext->getVisibilityListProperty();
        if(propPlacements && propCount->getValue()!=handle.getSize()) {
            int oldSize = handle.getSize();
            handle.setSize(propCount->getValue());
            for(int i=0;i<handle.getSize();++i) {
                Base::Matrix4D mat;
                if(propPlacements->getSize()>i) 
                    mat = (*propPlacements)[i].toMatrix();
                if(propScales && propScales->getSize()>i) {
                    Base::Matrix4D s;
                    s.scale((*propScales)[i]);
                    mat *= s;
                }
                handle.setTransform(i,mat);
                if(propElementVis && propElementVis->getSize()>i)
                    handle.setElementVisible(i,(*propElementVis)[i]);
                else
                    handle.setElementVisible(i,true);
            }
            if(!oldSize || !propCount->getValue())
                signalChangeIcon();
        }
    }else if(prop==ext->getScaleListProperty() || prop==ext->getPlacementListProperty()) {
        auto propPlacements = ext->getPlacementListProperty();
        auto propScales = ext->getScaleListProperty();
        if(propPlacements && handle.getSize()) {
            const auto &touched = 
                prop==propScales?propScales->getTouchList():propPlacements->getTouchList();
            if(touched.empty()) {
                for(int i=0;i<handle.getSize();++i) {
                    Base::Matrix4D mat;
                    if(propPlacements->getSize()>i) 
                        mat = (*propPlacements)[i].toMatrix();
                    if(propScales->getSize()>i) {
                        Base::Matrix4D s;
                        s.scale((*propScales)[i]);
                        mat *= s;
                    }
                    handle.setTransform(i,mat);
                }
            }else{
                for(int i : touched) {
                    if(i<0 || i>=handle.getSize())
                        continue;
                    Base::Matrix4D mat;
                    if(propPlacements->getSize()>i) 
                        mat = (*propPlacements)[i].toMatrix();
                    if(propScales->getSize()>i) {
                        Base::Matrix4D s;
                        s.scale((*propScales)[i]);
                        mat *= s;
                    }
                    handle.setTransform(i,mat);
                }
            }
        }
    }else if(prop == ext->getVisibilityListProperty()) {
        auto propElementVis = ext->getVisibilityListProperty();
        const auto &touched = propElementVis->getTouchList();
        if(touched.empty()) {
            for(int i=0;i<handle.getSize();++i) {
                if(propElementVis->getSize()>i)
                    handle.setElementVisible(i,(*propElementVis)[i]);
                else
                    handle.setElementVisible(i,true);
            }
        }else{
            for(int i : touched) {
                if(i>=0 && i<handle.getSize())
                    handle.setElementVisible(i,(*propElementVis)[i]);
            }
        }
    }else if(prop == ext->getElementListProperty()) {
        auto propElements = ext->getElementListProperty();
        if(!hasElements(ext)) {
            childrenCache.clear();
        }else{
            for(int i=0;i<propElements->getSize();++i) {
                auto obj = (*propElements)[i];
                auto vp = Application::Instance->getViewProvider(obj);
                if(vp && vp->isDerivedFrom(ViewProviderLinkElement::getClassTypeId())) {
                    auto vpe = static_cast<ViewProviderLinkElement*>(vp);
                    vpe->owner = this;
                    if(vpe->index!=i) {
                        vpe->index = i;
                        vpe->signalChangeIcon();
                    }
                    vpe->ShapeMaterial.setStatus(App::Property::User3,true);
                    if(MaterialList.getSize()>i)
                        vpe->ShapeMaterial.setValue(MaterialList[i]);
                    vpe->ShapeMaterial.setStatus(App::Property::User3,false);

                    vpe->UseMaterial.setStatus(App::Property::User3,true);
                    if(UseMaterialList.getSize()>i)
                        vpe->UseMaterial.setValue(UseMaterialList[i]);
                    else
                        vpe->UseMaterial.setValue(false);
                    vpe->UseMaterial.setStatus(App::Property::User3,false);
                }
            }
        }
        applyMaterial();
    }
}

void ViewProviderLink::applyMaterial() {
    if(!LinkUseMaterial.getValue()) return;
    handle.setMaterial(-1,&LinkShapeMaterial.getValue());
    for(int i=0;i<handle.getSize();++i) {
        if(MaterialList.getSize()>i && UseMaterialList.getSize()>i && UseMaterialList[i])
            handle.setMaterial(i,&MaterialList[i]);
    }
}

void ViewProviderLink::finishRestoring() {
    FC_TRACE("finish restoring");
    auto ext = getLinkExtension();
    if(!ext) return;
    App::Property *prop = ext->getLinkedObjectProperty();
    updateDataPrivate(ext,prop);
    if(ext->getLinkPlacementProperty())
        updateDataPrivate(ext,ext->getLinkPlacementProperty());
    else
        updateDataPrivate(ext,ext->getPlacementProperty());
    updateDataPrivate(ext,ext->getElementCountProperty());
    updateDataPrivate(ext,ext->getElementListProperty());

    // TODO: notify the tree. This is ugly, any other way?
    getObject()->getDocument()->signalChangedObject(*getObject(),ext->_LinkRecomputed);
    updateElementChildren(ext);
}

bool ViewProviderLink::hasElements(const App::LinkBaseExtension *ext) const {
    if(!ext) {
        ext = getLinkExtension();
        if(!ext) return false;
    }
    auto propElements = ext->getElementListProperty();
    return handle.getSize() && propElements && propElements->getSize()==handle.getSize();
}

std::vector<App::DocumentObject*> ViewProviderLink::claimChildren(void) const {
    auto ext = getLinkExtension();
    if(hasElements(ext))
        return ext->getElementListValue();
    if(handle.getSize()) {
        // in array mode without element objects, we'd better not show the
        // linked object's children to avoid inconsistent behavior on selection
        return std::vector<App::DocumentObject*>();
    }
    return claimChildrenPrivate();
}

std::vector<App::DocumentObject*> ViewProviderLink::claimChildrenPrivate(void) const {
    auto linked = getLinkedView(true);
    if(linked!=this)
        return linked->claimChildren();
    return std::vector<App::DocumentObject*>();
}

bool ViewProviderLink::canDragObject(App::DocumentObject* obj) const {
    auto ext = getLinkExtension();
    if(!ext || !ext->getLinkedObjectProperty() || hasElements(ext))
        return false;
    auto linked = getLinkedView(true);
    if(linked!=this)
        return linked->canDragObject(obj);
    return false;
}

bool ViewProviderLink::canDragObjects() const {
    auto ext = getLinkExtension();
    if(!ext || !ext->getLinkedObjectProperty() || hasElements(ext))
        return false;
    auto linked = getLinkedView(true);
    if(linked!=this)
        return linked->canDragObjects();
    return false;
}

void ViewProviderLink::dragObject(App::DocumentObject* obj) {
    if(hasElements()) return;
    auto linked = getLinkedView(true);
    if(linked!=this)
        linked->dragObject(obj);
}

bool ViewProviderLink::canDropObjects() const {
    auto ext = getLinkExtension();
    if(!ext || !ext->getLinkedObjectProperty() || hasElements(ext))
        return false;
    auto linked = getLinkedView(true);
    if(linked!=this)
        return linked->canDropObjects();
    return true;
}

bool ViewProviderLink::canDropObjectEx(App::DocumentObject *obj, 
        App::DocumentObject *owner, const char *subname) const
{
    auto ext = getLinkExtension();
    if(!ext || !ext->getLinkedObjectProperty() || hasElements(ext))
        return false;

    if(handle.isLinked()) {
        auto linked = getLinkedView(true);
        if(linked!=this)
            return linked->canDropObjectEx(obj,owner,subname);
    }
    if(obj->getDocument() != getObject()->getDocument() && 
       !dynamic_cast<App::PropertyXLink*>(ext->getLinkedObjectValue()))
        return false;

    return true;
}

void ViewProviderLink::dropObjectEx(App::DocumentObject* obj, 
        App::DocumentObject *owner, const char *subname) 
{
    auto ext = getLinkExtension();
    if(!ext || !ext->getLinkedObjectProperty() || hasElements(ext))
        return;

    auto linked = getLinkedView(true);
    if(linked == this) 
        ext->setLink(owner,subname);
    else
        linked->dropObjectEx(obj,owner,subname);
}

bool ViewProviderLink::canDragAndDropObject(App::DocumentObject* obj) const {
    auto ext = getLinkExtension();
    if(!ext || !ext->getLinkedObjectProperty() || hasElements(ext))
        return false;
    auto linked = getLinkedView(true);
    if(linked!=this) 
        return linked->canDragAndDropObject(obj);
    return false;
}

bool ViewProviderLink::getElementPicked(const SoPickedPoint *pp, std::string &subname) const {
    auto ext = getLinkExtension();
    if(!ext) return false;
    bool ret = handle.linkGetElementPicked(pp,subname);
    if(ret && hasElements(ext)) {
        auto propElements = ext->getElementListProperty();
        const char *sub = 0;
        int idx = App::LinkBaseExtension::getArrayIndex(subname.c_str(),&sub);
        assert(idx>=0 && idx<propElements->getSize());
        if(*sub) {
            --sub;
            assert(*sub == '.');
        }
        subname.replace(0,sub-subname.c_str(),(*propElements)[idx]->getNameInDocument());
    }
    return ret;
}

ViewProviderDocumentObject *ViewProviderLink::getLinkedView(bool recursive, int depth) const{
    auto ret = handle.linkGetLinkedView(recursive,depth);
    if(ret) return ret;
    return const_cast<ViewProviderLink*>(this);
}

bool ViewProviderLink::hasChildElement() const {
    if(hasElements())
        return true;
    auto linked = getLinkedView(true);
    if(linked && linked!=this) {
        if(linked->hasChildElement())
            return true;
        auto obj = linked->getObject();
        if(obj && obj->getNameInDocument())
            return obj->hasExtension(App::GroupExtension::getExtensionClassTypeId(),true);
    }
    return false;
}

SoDetail* ViewProviderLink::getDetailPath(
        const char *subname, SoFullPath *pPath, bool append) const 
{
    auto ext = getLinkExtension();
    if(!ext) return 0;

    auto len = pPath->getLength();
    if(append) {
        appendPath(pPath,pcRoot);
        appendPath(pPath,pcModeSwitch);
    }
    SoDetail *det = 0;
    std::string _subname;
    if(subname && subname[0] && hasElements(ext)) {
        int index = ext->getElementIndex(subname,&subname);
        if(index>=0) {
            char idx[40];
            snprintf(idx,sizeof(idx),"%d.",index);
            _subname = idx;
            _subname += subname;
            subname = _subname.c_str();
        }
    }
    if(handle.linkGetDetailPath(subname,pPath,det))
        return det;
    pPath->truncate(len);
    return 0;
}

int ViewProviderLink::isElementVisible(const char *element) const {
    auto ext = getLinkExtension();
    if(ext && hasElements(ext)){
        auto propElementVis = ext->getVisibilityListProperty();
        int index;
        if(propElementVis && element && element[0] && 
            ext->getElementListProperty()->find(element,&index))
        {
            return handle.isElementVisible(index)?1:0;
        }
        return -1;
    }
    auto ret = getLinkedView(true);
    if(ret && ret!=this)
        return ret->isElementVisible(element);
    return inherited::isElementVisible(element);
}

int ViewProviderLink::setElementVisible(const char *element, bool visible) {
    auto ext = getLinkExtension();
    if(hasElements(ext)) {
        auto propElementVis = ext->getVisibilityListProperty();
        if(!propElementVis || !element || !element[0]) 
            return -1;
        int index;
        auto propElements = ext->getElementListProperty();
        if(!propElements || !propElements->find(element,&index))
            return 0;
        if(propElementVis->getSize()<=index) {
            if(visible) return 1;
            propElementVis->setSize(index+1, true);
        }
        propElementVis->set1Value(index,visible,true);
        return 1;
    }
    auto ret = getLinkedView(true);
    if(ret && ret!=this)
        return ret->setElementVisible(element,visible);
    return inherited::setElementVisible(element,visible);
}

///////////////////////////////////////////////////////////////////////////////////

PROPERTY_SOURCE(Gui::ViewProviderLinkElement, Gui::ViewProviderDocumentObject)
ViewProviderLinkElement::ViewProviderLinkElement()
    :owner(0),index(-1)
{
    ADD_PROPERTY_TYPE(ShapeMaterial, (App::Material(App::Material::DEFAULT)), " Link", 
            App::PropertyType(App::Prop_Transient), 0);
    ShapeMaterial.setStatus(App::Property::MaterialEdit, true);
    ADD_PROPERTY_TYPE(UseMaterial, (false), " Link", App::PropertyType(App::Prop_Transient), 0);
    DisplayMode.setStatus(App::Property::Status::Hidden, true);
}

void ViewProviderLinkElement::onChanged(const App::Property* prop) {
    if(!isRestoring()) {
        if (prop == &ShapeMaterial) {
            if(owner) {
                if(!ShapeMaterial.testStatus(App::Property::User3)) {

                    UseMaterial.setStatus(App::Property::User3,true);
                    if(owner->UseMaterialList.getSize()<=index) 
                        owner->UseMaterialList.setSize(index+1,false);
                    owner->UseMaterialList.set1Value(index,true,false);
                    UseMaterial.setStatus(App::Property::User3,false);

                    owner->MaterialList.setStatus(App::Property::User3,true);
                    if(owner->MaterialList.getSize()<=index) 
                        owner->MaterialList.setSize(index+1,App::Material(App::Material::DEFAULT));
                    owner->MaterialList.set1Value(index,ShapeMaterial.getValue(),true);
                    owner->MaterialList.setStatus(App::Property::User3,false);

                    if(!owner->LinkUseMaterial.getValue())
                        owner->LinkUseMaterial.setValue(true);
                }
            }
        }else if(prop == &UseMaterial) {
            if(owner) {
                if(!UseMaterial.testStatus(App::Property::User3)) {
                    owner->UseMaterialList.setStatus(App::Property::User3,true);
                    if(UseMaterial.getValue()) {
                        if(owner->UseMaterialList.getSize()>index) 
                            owner->UseMaterialList.set1Value(index,true,true);
                    }else {
                        if(owner->UseMaterialList.getSize()<=index)
                            owner->UseMaterialList.setSize(index+1);
                        owner->UseMaterialList.set1Value(index,false,true);
                    }
                    owner->UseMaterialList.setStatus(App::Property::User3,false);

                    if(UseMaterial.getValue() && !owner->LinkUseMaterial.getValue())
                        owner->LinkUseMaterial.setValue(true);
                }
            }
        }
    }
    inherited::updateData(prop);
}

ViewProviderDocumentObject *ViewProviderLinkElement::getLinkedView(bool recursive, int depth) const {
    if(owner) {
        auto ret = owner->getLinkedView(recursive,depth);
        if(ret) return ret;
    }
    return const_cast<ViewProviderLinkElement*>(this);
}

QIcon ViewProviderLinkElement::getIcon(void) const {
    if(owner) {
        QIcon icon = owner->getLinkedIcon();
        if(!icon.isNull()) return icon;
    }
    char name[64];
    snprintf(name,sizeof(name),"LinkElement%d",index<0?0:index%3);
    return Gui::BitmapFactory().pixmap(name);
}

std::vector<App::DocumentObject*> ViewProviderLinkElement::claimChildren(void) const {
    if(owner)
        return owner->childrenCache;
    return std::vector<App::DocumentObject*>();
}

////////////////////////////////////////////////////////////////////////////////////////

namespace Gui {
PROPERTY_SOURCE_TEMPLATE(Gui::ViewProviderLinkPython, Gui::ViewProviderLink)
template class GuiExport ViewProviderPythonFeatureT<ViewProviderLink>;
}
