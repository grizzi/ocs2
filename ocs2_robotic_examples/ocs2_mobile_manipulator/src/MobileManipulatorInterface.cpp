/******************************************************************************
Copyright (c) 2020, Farbod Farshidian. All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

 * Redistributions of source code must retain the above copyright notice, this
  list of conditions and the following disclaimer.

 * Redistributions in binary form must reproduce the above copyright notice,
  this list of conditions and the following disclaimer in the documentation
  and/or other materials provided with the distribution.

 * Neither the name of the copyright holder nor the names of its
  contributors may be used to endorse or promote products derived from
  this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
******************************************************************************/

#include <string>

#include <pinocchio/fwd.hpp>  // forward declarations must be included first.

#include <pinocchio/multibody/joint/joint-composite.hpp>
#include <pinocchio/multibody/model.hpp>

#include "ocs2_mobile_manipulator/MobileManipulatorInterface.h"

#include <ocs2_core/initialization/DefaultInitializer.h>
#include <ocs2_core/misc/LoadData.h>
#include <ocs2_core/soft_constraint/StateInputSoftConstraint.h>
#include <ocs2_core/soft_constraint/StateSoftConstraint.h>
#include <ocs2_core/soft_constraint/penalties/DoubleSidedPenalty.h>
#include <ocs2_core/soft_constraint/penalties/QuadraticPenalty.h>
#include <ocs2_core/soft_constraint/penalties/RelaxedBarrierPenalty.h>
#include <ocs2_oc/synchronized_module/ReferenceManager.h>
#include <ocs2_pinocchio_interface/PinocchioEndEffectorKinematics.h>
#include <ocs2_pinocchio_interface/PinocchioEndEffectorKinematicsCppAd.h>
#include <ocs2_pinocchio_interface/urdf.h>
#include <ocs2_self_collision/SelfCollisionConstraint.h>
#include <ocs2_self_collision/SelfCollisionConstraintCppAd.h>
#include <ocs2_self_collision/loadStdVectorOfPair.h>

#include "ocs2_mobile_manipulator/MobileManipulatorDynamics.h"
#include "ocs2_mobile_manipulator/MobileManipulatorPreComputation.h"
#include "ocs2_mobile_manipulator/constraint/EndEffectorConstraint.h"
#include "ocs2_mobile_manipulator/constraint/JointVelocityLimits.h"
#include "ocs2_mobile_manipulator/constraint/MobileManipulatorSelfCollisionConstraint.h"
#include "ocs2_mobile_manipulator/cost/QuadraticInputCost.h"

// Boost
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>

namespace ocs2 {
namespace mobile_manipulator {

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
MobileManipulatorInterface::MobileManipulatorInterface(const std::string& taskFile, const std::string& libraryFolder,
                                                       const std::string& urdfFile) {
  // check that task file exists
  boost::filesystem::path taskFilePath(taskFile);
  if (boost::filesystem::exists(taskFilePath)) {
    std::cerr << "[MobileManipulatorInterface] Loading task file: " << taskFilePath << std::endl;
  } else {
    throw std::invalid_argument("[MobileManipulatorInterface] Task file not found: " + taskFilePath.string());
  }
  // check that urdf file exists
  boost::filesystem::path urdfFilePath(urdfFile);
  if (boost::filesystem::exists(urdfFilePath)) {
    std::cerr << "[MobileManipulatorInterface] Loading Pinocchio model from: " << urdfFilePath << std::endl;
  } else {
    throw std::invalid_argument("[MobileManipulatorInterface] URDF file not found: " + urdfFilePath.string());
  }
  // create library folder if it does not exist
  boost::filesystem::path libraryFolderPath(libraryFolder);
  boost::filesystem::create_directories(libraryFolderPath);
  std::cerr << "[MobileManipulatorInterface] Generated library path: " << libraryFolderPath << std::endl;

  // create pinocchio interface
  pinocchioInterfacePtr_.reset(new PinocchioInterface(buildPinocchioInterface(urdfFile)));
  std::cerr << *pinocchioInterfacePtr_;

  bool usePreComputation = true;
  bool recompileLibraries = true;
  boost::property_tree::ptree pt;
  boost::property_tree::read_info(taskFile, pt);
  std::cerr << "\n #### Model Settings:";
  std::cerr << "\n #### =============================================================================\n";
  loadData::loadPtreeValue(pt, usePreComputation, "model_settings.usePreComputation", true);
  loadData::loadPtreeValue(pt, recompileLibraries, "model_settings.recompileLibraries", true);
  std::cerr << " #### =============================================================================\n";

  // Default initial state
  loadData::loadEigenMatrix(taskFile, "initialState", initialState_);
  std::cerr << "Initial State:   " << initialState_.transpose() << std::endl;

  // DDP-MPC settings
  ddpSettings_ = ddp::loadSettings(taskFile, "ddp");
  mpcSettings_ = mpc::loadSettings(taskFile, "mpc");

  // Reference Manager
  referenceManagerPtr_.reset(new ReferenceManager);

  /*
   * Optimal control problem
   */
  // Cost
  problem_.costPtr->add("inputCost", getQuadraticInputCost(taskFile));

  // Constraints
  problem_.softConstraintPtr->add("jointVelocityLimit", getJointVelocityLimitConstraint(taskFile));
  problem_.stateSoftConstraintPtr->add("selfCollision", getSelfCollisionConstraint(*pinocchioInterfacePtr_, taskFile, urdfFile,
                                                                                   usePreComputation, libraryFolder, recompileLibraries));
  problem_.stateSoftConstraintPtr->add("enfEffector", getEndEffectorConstraint(*pinocchioInterfacePtr_, taskFile, "endEffector",
                                                                               usePreComputation, libraryFolder, recompileLibraries));
  problem_.finalSoftConstraintPtr->add("finalEndEffector", getEndEffectorConstraint(*pinocchioInterfacePtr_, taskFile, "finalEndEffector",
                                                                                    usePreComputation, libraryFolder, recompileLibraries));

  // Dynamics
  problem_.dynamicsPtr.reset(new MobileManipulatorDynamics("mobile_manipulator_dynamics", libraryFolder, recompileLibraries, true));

  /*
   * Pre-computation
   */
  if (usePreComputation) {
    problem_.preComputationPtr.reset(new MobileManipulatorPreComputation(*pinocchioInterfacePtr_));
  }

  // Rollout
  const auto rolloutSettings = rollout::loadSettings(taskFile, "rollout");
  rolloutPtr_.reset(new TimeTriggeredRollout(*problem_.dynamicsPtr, rolloutSettings));

  // Initialization
  initializerPtr_.reset(new DefaultInitializer(INPUT_DIM));
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
PinocchioInterface MobileManipulatorInterface::buildPinocchioInterface(const std::string& urdfFile) {
  // add 3 DOF for wheelbase
  pinocchio::JointModelComposite rootJoint(3);
  rootJoint.addJoint(pinocchio::JointModelPX());
  rootJoint.addJoint(pinocchio::JointModelPY());
  rootJoint.addJoint(pinocchio::JointModelRZ());

  return getPinocchioInterfaceFromUrdfFile(urdfFile, rootJoint);
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
std::unique_ptr<StateInputCost> MobileManipulatorInterface::getQuadraticInputCost(const std::string& taskFile) {
  matrix_t R(INPUT_DIM, INPUT_DIM);

  std::cerr << "\n #### Input Cost Settings: ";
  std::cerr << "\n #### =============================================================================\n";
  loadData::loadEigenMatrix(taskFile, "inputCost.R", R);
  std::cerr << "inputCost.R:  \n" << R << '\n';
  std::cerr << " #### =============================================================================\n";

  return std::unique_ptr<StateInputCost>(new QuadraticInputCost(std::move(R)));
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
std::unique_ptr<StateCost> MobileManipulatorInterface::getEndEffectorConstraint(const PinocchioInterface& pinocchioInterface,
                                                                                const std::string& taskFile, const std::string& prefix,
                                                                                bool usePreComputation, const std::string& libraryFolder,
                                                                                bool recompileLibraries) {
  scalar_t muPosition = 1.0;
  scalar_t muOrientation = 1.0;
  const std::string name = "WRIST_2";

  boost::property_tree::ptree pt;
  boost::property_tree::read_info(taskFile, pt);
  std::cerr << "\n #### " << prefix << " Settings: ";
  std::cerr << "\n #### =============================================================================\n";
  loadData::loadPtreeValue(pt, muPosition, prefix + ".muPosition", true);
  loadData::loadPtreeValue(pt, muOrientation, prefix + ".muOrientation", true);
  std::cerr << " #### =============================================================================\n";

  if (referenceManagerPtr_ == nullptr) {
    throw std::runtime_error("[getEndEffectorConstraint] referenceManagerPtr_ should be set first!");
  }

  std::unique_ptr<StateConstraint> constraint;
  if (usePreComputation) {
    MobileManipulatorPinocchioMapping<scalar_t> pinocchioMapping;
    PinocchioEndEffectorKinematics eeKinematics(pinocchioInterface, pinocchioMapping, {name});
    constraint.reset(new EndEffectorConstraint(eeKinematics, *referenceManagerPtr_));
  } else {
    MobileManipulatorPinocchioMapping<ad_scalar_t> pinocchioMappingCppAd;
    PinocchioEndEffectorKinematicsCppAd eeKinematics(pinocchioInterface, pinocchioMappingCppAd, {name}, STATE_DIM, INPUT_DIM,
                                                     "end_effector_kinematics", libraryFolder, recompileLibraries, false);
    constraint.reset(new EndEffectorConstraint(eeKinematics, *referenceManagerPtr_));
  }

  std::vector<std::unique_ptr<PenaltyBase>> penaltyArray(6);
  std::generate_n(penaltyArray.begin(), 3, [&] { return std::unique_ptr<PenaltyBase>(new QuadraticPenalty(muPosition)); });
  std::generate_n(penaltyArray.begin() + 3, 3, [&] { return std::unique_ptr<PenaltyBase>(new QuadraticPenalty(muOrientation)); });

  return std::unique_ptr<StateCost>(new StateSoftConstraint(std::move(constraint), std::move(penaltyArray)));
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
std::unique_ptr<StateCost> MobileManipulatorInterface::getSelfCollisionConstraint(const PinocchioInterface& pinocchioInterface,
                                                                                  const std::string& taskFile, const std::string& urdfFile,
                                                                                  bool usePreComputation, const std::string& libraryFolder,
                                                                                  bool recompileLibraries) {
  std::vector<std::pair<size_t, size_t>> collisionObjectPairs;
  std::vector<std::pair<std::string, std::string>> collisionLinkPairs;
  scalar_t mu = 1e-2;
  scalar_t delta = 1e-3;
  scalar_t minimumDistance = 0.0;

  boost::property_tree::ptree pt;
  boost::property_tree::read_info(taskFile, pt);
  const std::string prefix = "selfCollision.";
  std::cerr << "\n #### SelfCollision Settings: ";
  std::cerr << "\n #### =============================================================================\n";
  loadData::loadPtreeValue(pt, mu, prefix + "mu", true);
  loadData::loadPtreeValue(pt, delta, prefix + "delta", true);
  loadData::loadPtreeValue(pt, minimumDistance, prefix + "minimumDistance", true);
  loadData::loadStdVectorOfPair(taskFile, prefix + "collisionObjectPairs", collisionObjectPairs, true);
  loadData::loadStdVectorOfPair(taskFile, prefix + "collisionLinkPairs", collisionLinkPairs, true);
  std::cerr << " #### =============================================================================\n";

  PinocchioGeometryInterface geometryInterface(pinocchioInterface, collisionLinkPairs, collisionObjectPairs);

  const size_t numCollisionPairs = geometryInterface.getNumCollisionPairs();
  std::cerr << "SelfCollision: Testing for " << numCollisionPairs << " collision pairs\n";

  std::unique_ptr<StateConstraint> constraint;
  if (usePreComputation) {
    constraint = std::unique_ptr<StateConstraint>(new MobileManipulatorSelfCollisionConstraint(
        MobileManipulatorPinocchioMapping<scalar_t>(), std::move(geometryInterface), minimumDistance));
  } else {
    constraint = std::unique_ptr<StateConstraint>(
        new SelfCollisionConstraintCppAd(pinocchioInterface, MobileManipulatorPinocchioMapping<scalar_t>(), std::move(geometryInterface),
                                         minimumDistance, "self_collision", libraryFolder, recompileLibraries, false));
  }

  std::unique_ptr<PenaltyBase> penalty(new RelaxedBarrierPenalty({mu, delta}));

  return std::unique_ptr<StateCost>(new StateSoftConstraint(std::move(constraint), std::move(penalty)));
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
std::unique_ptr<StateInputCost> MobileManipulatorInterface::getJointVelocityLimitConstraint(const std::string& taskFile) {
  vector_t lowerBound(INPUT_DIM);
  vector_t upperBound(INPUT_DIM);
  scalar_t mu = 1e-2;
  scalar_t delta = 1e-3;

  boost::property_tree::ptree pt;
  boost::property_tree::read_info(taskFile, pt);
  const std::string prefix = "jointVelocityLimits.";
  std::cerr << "\n #### JointVelocityLimits Settings: ";
  std::cerr << "\n #### =============================================================================\n";
  loadData::loadEigenMatrix(taskFile, "jointVelocityLimits.lowerBound", lowerBound);
  std::cerr << " #### 'lowerBound':  " << lowerBound.transpose() << std::endl;
  loadData::loadEigenMatrix(taskFile, "jointVelocityLimits.upperBound", upperBound);
  std::cerr << " #### 'upperBound':  " << upperBound.transpose() << std::endl;
  loadData::loadPtreeValue(pt, mu, prefix + "mu", true);
  loadData::loadPtreeValue(pt, delta, prefix + "delta", true);
  std::cerr << " #### =============================================================================\n";

  std::unique_ptr<StateInputConstraint> constraint(new JointVelocityLimits);

  std::unique_ptr<PenaltyBase> barrierFunction;
  std::vector<std::unique_ptr<PenaltyBase>> penaltyArray(INPUT_DIM);
  for (int i = 0; i < INPUT_DIM; i++) {
    barrierFunction.reset(new RelaxedBarrierPenalty({mu, delta}));
    penaltyArray[i].reset(new DoubleSidedPenalty(lowerBound(i), upperBound(i), std::move(barrierFunction)));
  }

  return std::unique_ptr<StateInputCost>(new StateInputSoftConstraint(std::move(constraint), std::move(penaltyArray)));
}

}  // namespace mobile_manipulator
}  // namespace ocs2
