// Copyright (c) Facebook, Inc. and its affiliates.
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include <Magnum/BulletIntegration/DebugDraw.h>
#include <Magnum/BulletIntegration/Integration.h>

#include <Corrade/Utility/Assert.h>

#include <utility>

#include "BulletCollision/CollisionShapes/btCompoundShape.h"
#include "BulletCollision/CollisionShapes/btConvexHullShape.h"
#include "BulletCollision/CollisionShapes/btConvexTriangleMeshShape.h"
#include "BulletCollision/Gimpact/btGImpactShape.h"
#include "BulletCollision/NarrowPhaseCollision/btRaycastCallback.h"
#include "BulletDebugManager.h"
#include "BulletRigidObject.h"

//!  A Few considerations in construction
//!  Bullet Mesh conversion adapted from:
//!      https://github.com/mosra/magnum-integration/issues/20
//!      https://pybullet.org/Bullet/phpBB3/viewtopic.php?t=11001
//!  Bullet object margin (p15):
//!      https://facultyfp.salisbury.edu/despickler/personal/Resources/
//!        GraphicsExampleCodeGLSL_SFML/InterfaceDoc/Bullet/Bullet_User_Manual.pdf
//!      It's okay to set margin down to 1mm
//!        (1) Bullet/MJCF example
//!      Another solution:
//!        (1) Keep 4cm margin
//!        (2) Use examples/Importers/ImportBsp

namespace esp {
namespace physics {

BulletRigidObject::BulletRigidObject(
    scene::SceneNode* rigidBodyNode,
    int objectId,
    const assets::ResourceManager& resMgr,
    std::shared_ptr<btMultiBodyDynamicsWorld> bWorld,
    std::shared_ptr<std::map<const btCollisionObject*, int> >
        collisionObjToObjIds)
    : BulletBase(std::move(bWorld), std::move(collisionObjToObjIds)),
      RigidObject(rigidBodyNode, objectId, resMgr),
      MotionState(*rigidBodyNode) {}

BulletRigidObject::~BulletRigidObject() {
  if (!isActive()) {
    // This object may be supporting other sleeping objects, so wake them before
    // removing.
    activateCollisionIsland();
  }

  // remove rigid body from the world
  bWorld_->removeRigidBody(bObjectRigidBody_.get());

  collisionObjToObjIds_->erase(bObjectRigidBody_.get());

}  //~BulletRigidObject

bool BulletRigidObject::initialization_LibSpecific() {
  // TODO: add is_dynamic flag
  objectMotionType_ = MotionType::DYNAMIC;

  isCollidable_ = getInitializationAttributes()->getIsCollidable();

  // create the bObjectRigidBody_
  constructAndAddRigidBody(objectMotionType_);

  return true;
}  // initialization_LibSpecific

bool BulletRigidObject::finalizeObject_LibSpecific() {
  if (usingBBCollisionShape_) {
    setCollisionFromBB();
  }
  return true;
}  // finalizeObject_LibSpecifc

bool BulletRigidObject::constructCollisionShape() {
  // get this object's creation template, appropriately cast
  auto tmpAttr = getInitializationAttributes();

  //! Physical parameters
  double margin = tmpAttr->getMargin();
  bool joinCollisionMeshes = tmpAttr->getJoinCollisionMeshes();
  usingBBCollisionShape_ = tmpAttr->getBoundingBoxCollisions();

  // TODO(alexanderwclegg): should provide the option for joinCollisionMeshes
  // and collisionFromBB_ to specify complete vs. component level bounding box
  // heirarchies.

  //! Iterate through all mesh components for one object
  //! The components are combined into a convex compound shape
  bObjectShape_ = std::make_unique<btCompoundShape>();
  // collision mesh/asset handle
  const std::string collisionAssetHandle =
      initializationAttributes_->getCollisionAssetHandle();

  if (!initializationAttributes_->getUseMeshCollision()) {
    // if using prim collider get appropriate bullet collision primitive
    // attributes and build bullet collision shape
    auto primAttributes =
        resMgr_.getAssetAttributesManager()->getObjectCopyByHandle(
            collisionAssetHandle);
    // primitive object pointer construction
    auto primObjPtr = buildPrimitiveCollisionObject(
        primAttributes->getPrimObjType(), primAttributes->getHalfLength());
    if (nullptr == primObjPtr) {
      bObjectShape_.reset();
      return false;
    }
    primObjPtr->setLocalScaling(btVector3(tmpAttr->getCollisionAssetSize()));
    bGenericShapes_.clear();
    bGenericShapes_.emplace_back(std::move(primObjPtr));
    bObjectShape_->addChildShape(btTransform::getIdentity(),
                                 bGenericShapes_.back().get());
    bObjectShape_->recalculateLocalAabb();
  } else {
    // mesh collider
    const std::vector<assets::CollisionMeshData>& meshGroup =
        resMgr_.getCollisionMesh(collisionAssetHandle);
    const assets::MeshMetaData& metaData =
        resMgr_.getMeshMetaData(collisionAssetHandle);

    if (!usingBBCollisionShape_) {
      constructConvexShapesFromMeshes(Magnum::Matrix4{}, meshGroup,
                                      metaData.root, joinCollisionMeshes,
                                      bObjectShape_.get());

      // add the final object after joining meshes
      if (joinCollisionMeshes) {
        bObjectConvexShapes_.back()->setLocalScaling(
            btVector3(tmpAttr->getCollisionAssetSize()));
        bObjectConvexShapes_.back()->setMargin(0.0);
        bObjectConvexShapes_.back()->recalcLocalAabb();
        bObjectShape_->addChildShape(btTransform::getIdentity(),
                                     bObjectConvexShapes_.back().get());
      }
    }
  }  // if using prim collider else use mesh collider

  //! Set properties
  bObjectShape_->setMargin(margin);

  bObjectShape_->setLocalScaling(btVector3{tmpAttr->getScale()});
  bObjectShape_->recalculateLocalAabb();

  if (!originShift_.isZero()) {
    // for deferred creation of the shape
    shiftObjectCollisionShape(originShift_);
  }

  return true;
}

std::unique_ptr<btCollisionShape>
BulletRigidObject::buildPrimitiveCollisionObject(int primTypeVal,
                                                 double halfLength) {
  // int primTypeVal = primAttributes.getPrimObjType();
  CORRADE_ASSERT(
      (primTypeVal >= 0) &&
          (primTypeVal <
           static_cast<int>(metadata::PrimObjTypes::END_PRIM_OBJ_TYPES)),
      "BulletRigidObject::buildPrimitiveCollisionObject : Illegal primitive "
      "value requested : "
          << primTypeVal,
      nullptr);
  metadata::PrimObjTypes primType =
      static_cast<metadata::PrimObjTypes>(primTypeVal);

  std::unique_ptr<btCollisionShape> obj(nullptr);
  switch (primType) {
    case metadata::PrimObjTypes::CAPSULE_SOLID:
    case metadata::PrimObjTypes::CAPSULE_WF: {
      // use bullet capsule :  btCapsuleShape(btScalar radius,btScalar height);
      btScalar radius = 1.0f;
      btScalar height = 2.0 * halfLength;
      obj = std::make_unique<btCapsuleShape>(radius, height);
      break;
    }
    case metadata::PrimObjTypes::CONE_SOLID:
    case metadata::PrimObjTypes::CONE_WF: {
      // use bullet cone : btConeShape(btScalar radius,btScalar height);
      btScalar radius = 1.0f;
      btScalar height = 2.0 * halfLength;
      obj = std::make_unique<btConeShape>(radius, height);
      break;
    }
    case metadata::PrimObjTypes::CUBE_SOLID:
    case metadata::PrimObjTypes::CUBE_WF: {
      // use bullet box shape : btBoxShape( const btVector3& boxHalfExtents);
      btVector3 dim(1.0, 1.0, 1.0);
      obj = std::make_unique<btBoxShape>(dim);
      break;
    }
    case metadata::PrimObjTypes::CYLINDER_SOLID:
    case metadata::PrimObjTypes::CYLINDER_WF: {
      // use bullet cylinder shape :btCylinderShape (const btVector3&
      // halfExtents);
      btVector3 dim(1.0, 1.0, 1.0);
      obj = std::make_unique<btCylinderShape>(dim);
      break;
    }
    case metadata::PrimObjTypes::ICOSPHERE_SOLID:
    case metadata::PrimObjTypes::ICOSPHERE_WF:
    case metadata::PrimObjTypes::UVSPHERE_SOLID:
    case metadata::PrimObjTypes::UVSPHERE_WF: {
      // use bullet sphere shape :btSphereShape (btScalar radius)
      btScalar radius = 1.0f;
      obj = std::make_unique<btSphereShape>(radius);
      break;
    }
    default: {
      return nullptr;
    }
  }  // switch
  // set primitive object shape margin to be 0, set margin in actual
  // (potentially compound) object
  obj->setMargin(0.0);
  return obj;
}  // buildPrimitiveCollisionObject

void BulletRigidObject::setCollisionFromBB() {
  btVector3 dim(node().getCumulativeBB().size() / 2.0);

  if (!bObjectShape_) {
    bObjectShape_ = std::make_unique<btCompoundShape>();
  }

  for (auto& shape : bGenericShapes_) {
    bObjectShape_->removeChildShape(shape.get());
  }
  bGenericShapes_.clear();
  bGenericShapes_.emplace_back(std::make_unique<btBoxShape>(dim));
  bObjectShape_->addChildShape(btTransform::getIdentity(),
                               bGenericShapes_.back().get());
  bObjectShape_->recalculateLocalAabb();

  if (isCollidable_) {
    // otherwise this setup is deferred
    bObjectRigidBody_->setCollisionShape(bObjectShape_.get());

    auto tmpAttr = getInitializationAttributes();
    btVector3 bInertia(tmpAttr->getInertia());
    if (bInertia == btVector3{0, 0, 0}) {
      // allow bullet to compute the inertia tensor if we don't have one
      bObjectShape_->calculateLocalInertia(getMass(),
                                           bInertia);  // overrides bInertia

      setInertiaVector(Magnum::Vector3(bInertia));
    }
  }
}  // setCollisionFromBB

bool BulletRigidObject::setMotionType(MotionType mt) {
  if (mt == MotionType::UNDEFINED) {
    return false;
  }
  if (mt == objectMotionType_) {
    return true;  // no work
  }

  // remove the existing object from the world to change its type
  bWorld_->removeRigidBody(bObjectRigidBody_.get());
  constructAndAddRigidBody(mt);
  objectMotionType_ = mt;
  return true;
}  // setMotionType

bool BulletRigidObject::setCollidable(bool collidable) {
  if (collidable == isCollidable_) {
    // no work
    return true;
  }

  isCollidable_ = collidable;
  if (!isCollidable_ && !isActive()) {
    activateCollisionIsland();
  }
  bWorld_->removeRigidBody(bObjectRigidBody_.get());
  constructAndAddRigidBody(objectMotionType_);

  return true;
}

void BulletRigidObject::shiftOrigin(const Magnum::Vector3& shift) {
  if (visualNode_)
    visualNode_->translate(shift);
  node().computeCumulativeBB();

  originShift_ = shift;

  if (bObjectShape_) {
    // otherwise deferred to construction
    shiftObjectCollisionShape(originShift_);
  }
}  // shiftOrigin

void BulletRigidObject::shiftObjectCollisionShape(
    const Magnum::Vector3& shift) {
  // shift all children of the parent collision shape
  for (int i = 0; i < bObjectShape_->getNumChildShapes(); i++) {
    btTransform cT = bObjectShape_->getChildTransform(i);
    cT.setOrigin(cT.getOrigin() + btVector3(shift));
    bObjectShape_->updateChildTransform(i, cT, false);
  }
  // recompute the Aabb once when done
  bObjectShape_->recalculateLocalAabb();
}

//! Synchronize Physics transformations
//! Needed after changing the pose from Magnum side
void BulletRigidObject::syncPose() {
  //! For syncing objects
  bObjectRigidBody_->setWorldTransform(
      btTransform(node().transformationMatrix()));
}  // syncPose

std::string BulletRigidObject::getCollisionDebugName() {
  return "RigidObject, " + initializationAttributes_->getHandle() + ", id " +
         std::to_string(objectId_);
}

void BulletRigidObject::constructAndAddRigidBody(MotionType mt) {
  // get this object's creation template, appropriately cast
  auto tmpAttr = getInitializationAttributes();

  if (bObjectShape_ == nullptr && isCollidable_) {
    constructCollisionShape();
  }

  double mass = 0;
  btVector3 bInertia = {0, 0, 0};
  if (mt == MotionType::DYNAMIC) {
    mass = tmpAttr->getMass();
    bInertia = btVector3(tmpAttr->getInertia());
    if (bInertia == btVector3{0, 0, 0}) {
      if (bObjectShape_ != nullptr) {
        // allow bullet to compute the inertia tensor if we don't have one
        bObjectShape_->calculateLocalInertia(mass,
                                             bInertia);  // overrides bInertia
      } else {
        // TODO: better default given object information?
        bInertia = btVector3(1.0, 1.0, 1.0);
      }
    }
  }

  //! Bullet rigid body setup
  auto motionState =
      (mt == MotionType::STATIC) ? nullptr : &(this->btMotionState());

  btRigidBody::btRigidBodyConstructionInfo info =
      btRigidBody::btRigidBodyConstructionInfo(mass, motionState,
                                               bObjectShape_.get(), bInertia);

  if (!isCollidable_) {
    // set an empty collision shape to disable collisions
    bEmptyShape_ = std::make_unique<btCompoundShape>();
    info.m_collisionShape = bEmptyShape_.get();
  }

  if (bObjectRigidBody_ != nullptr) {
    // set physical properties from possibly modified current rigidBody
    info.m_startWorldTransform = bObjectRigidBody_->getWorldTransform();
    info.m_friction = bObjectRigidBody_->getFriction();
    info.m_restitution = bObjectRigidBody_->getRestitution();
    info.m_linearDamping = bObjectRigidBody_->getLinearDamping();
    info.m_angularDamping = bObjectRigidBody_->getAngularDamping();
  } else {
    // set properties from initialization template
    info.m_friction = tmpAttr->getFrictionCoefficient();
    info.m_restitution = tmpAttr->getRestitutionCoefficient();
    info.m_linearDamping = tmpAttr->getLinearDamping();
    info.m_angularDamping = tmpAttr->getAngularDamping();
  }

  //! Create rigid body
  if (collisionObjToObjIds_->count(bObjectRigidBody_.get())) {
    collisionObjToObjIds_->erase(bObjectRigidBody_.get());
  }
  bObjectRigidBody_ = std::make_unique<btRigidBody>(info);
  collisionObjToObjIds_->emplace(bObjectRigidBody_.get(), objectId_);

  if (mt == MotionType::KINEMATIC) {
    bObjectRigidBody_->setCollisionFlags(
        bObjectRigidBody_->getCollisionFlags() |
        btCollisionObject::CF_KINEMATIC_OBJECT);
    CORRADE_INTERNAL_ASSERT(bObjectRigidBody_->isKinematicObject());
  }

  // add the object to the world
  if (mt == MotionType::STATIC) {
    CORRADE_INTERNAL_ASSERT(bObjectRigidBody_->isStaticObject());
    bWorld_->addRigidBody(
        bObjectRigidBody_.get(), int(CollisionGroup::Static),
        CollisionGroupHelper::getMaskForGroup(CollisionGroup::Static));
  } else {
    bWorld_->addRigidBody(
        bObjectRigidBody_.get(), int(CollisionGroup::FreeObject),
        CollisionGroupHelper::getMaskForGroup(CollisionGroup::FreeObject));
    setActive();
  }
}

void BulletRigidObject::activateCollisionIsland() {
  btCollisionObject* thisColObj = bObjectRigidBody_.get();

  // first query overlapping pairs of the current object from the most recent
  // broadphase to collect relevant simulation islands.
  std::set<int> overlappingSimulationIslands = {thisColObj->getIslandTag()};
  auto& pairCache =
      bWorld_->getCollisionWorld()->getPairCache()->getOverlappingPairArray();

  for (int i = 0; i < pairCache.size(); ++i) {
    if (pairCache.at(i).m_pProxy0->m_clientObject == thisColObj) {
      overlappingSimulationIslands.insert(
          static_cast<btCollisionObject*>(
              pairCache.at(i).m_pProxy1->m_clientObject)
              ->getIslandTag());
    } else if (pairCache.at(i).m_pProxy1->m_clientObject == thisColObj) {
      overlappingSimulationIslands.insert(
          static_cast<btCollisionObject*>(
              pairCache.at(i).m_pProxy0->m_clientObject)
              ->getIslandTag());
    }
  }

  // activate nearby objects in the simulation island as computed on the
  // previous collision detection pass
  auto& colObjs = bWorld_->getCollisionWorld()->getCollisionObjectArray();
  for (auto objIx = 0; objIx < colObjs.size(); ++objIx) {
    if (overlappingSimulationIslands.count(colObjs[objIx]->getIslandTag()) >
        0) {
      colObjs[objIx]->activate();
    }
  }
}

void BulletRigidObject::setCOM(const Magnum::Vector3&) {
  // Current not supported
  /*
    bObjectRigidBody_->setCenterOfMassTransform(
        btTransform(Magnum::Matrix4<float>::translation(COM)));*/
}  // setCOM

Magnum::Vector3 BulletRigidObject::getCOM() const {
  // TODO: double check the position if there is any implicit transformation
  // done

  const Magnum::Vector3 com =
      Magnum::Vector3(bObjectRigidBody_->getCenterOfMassPosition());
  return com;
}  // getCOM

Mn::Range3D BulletRigidObject::getAABB() const {
  btVector3 min, max;
  bObjectRigidBody_->getAabb(min, max);
  return {Mn::Vector3{min}, Mn::Vector3{max}};
}

bool BulletRigidObject::contactTest() {
  SimulationContactResultCallback src;
  bWorld_->getCollisionWorld()->contactTest(bObjectRigidBody_.get(), src);
  return src.bCollision;
}  // contactTest

const Magnum::Range3D BulletRigidObject::getCollisionShapeAabb() const {
  if (!bObjectShape_) {
    // e.g. empty scene
    return Magnum::Range3D();
  }
  btVector3 localAabbMin, localAabbMax;
  bObjectShape_->getAabb(btTransform::getIdentity(), localAabbMin,
                         localAabbMax);
  return Magnum::Range3D{Magnum::Vector3{localAabbMin},
                         Magnum::Vector3{localAabbMax}};
}  // getCollisionShapeAabb

bool BulletRigidObject::isMe(const btCollisionObject* collisionObject) {
  for (auto& sceneObj : bStaticCollisionObjects_) {
    if (sceneObj.get() == collisionObject) {
      return true;
    }
  }
  if (bObjectRigidBody_.get() == collisionObject) {
    return true;
  }

  return false;
}

void BulletRigidObject::overrideCollisionGroup(CollisionGroup group) {
  if (!bObjectRigidBody_->isInWorld()) {
    LOG(ERROR) << "BulletRigidObject::overrideCollisionGroup failed because "
                  "the Bullet body hasn't yet been added to the Bullet world.";
  }

  bWorld_->removeRigidBody(bObjectRigidBody_.get());
  bWorld_->addRigidBody(bObjectRigidBody_.get(), int(group),
                        CollisionGroupHelper::getMaskForGroup(group));
}

}  // namespace physics
}  // namespace esp
