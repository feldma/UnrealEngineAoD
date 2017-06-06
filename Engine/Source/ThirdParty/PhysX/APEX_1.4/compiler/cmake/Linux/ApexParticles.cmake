#
# Build APEX_Particles
#

SET(GW_DEPS_ROOT $ENV{GW_DEPS_ROOT})
FIND_PACKAGE(PxShared REQUIRED)

SET(APEX_MODULE_DIR ${PROJECT_SOURCE_DIR}/../../../module)

SET(AM_SOURCE_DIR ${APEX_MODULE_DIR}/particles)


# Use generator expressions to set config specific preprocessor definitions
SET(APEX_PARTICLES_COMPILE_DEFS 
	# Common to all configurations
	${APEX_LINUX_COMPILE_DEFS};_LIB;PX_PHYSX_STATIC_LIB;
)

if(${CMAKE_BUILD_TYPE_LOWERCASE} STREQUAL "debug")
	LIST(APPEND APEX_PARTICLES_COMPILE_DEFS 
		${APEX_LINUX_DEBUG_COMPILE_DEFS};PX_PHYSX_DLL_NAME_POSTFIX=DEBUG;
	)
elseif(${CMAKE_BUILD_TYPE_LOWERCASE} STREQUAL "checked")
	LIST(APPEND APEX_PARTICLES_COMPILE_DEFS 
		${APEX_LINUX_CHECKED_COMPILE_DEFS};PX_PHYSX_DLL_NAME_POSTFIX=CHECKED;
	)
elseif(${CMAKE_BUILD_TYPE_LOWERCASE} STREQUAL "profile")
	LIST(APPEND APEX_PARTICLES_COMPILE_DEFS 
		${APEX_LINUX_PROFILE_COMPILE_DEFS};PX_PHYSX_DLL_NAME_POSTFIX=PROFILE;
	)
elseif(${CMAKE_BUILD_TYPE_LOWERCASE} STREQUAL release)
	LIST(APPEND APEX_PARTICLES_COMPILE_DEFS 
		${APEX_LINUX_RELEASE_COMPILE_DEFS}
	)
else(${CMAKE_BUILD_TYPE_LOWERCASE} STREQUAL "debug")
	MESSAGE(FATAL_ERROR "Unknown configuration ${CMAKE_BUILD_TYPE}")
endif(${CMAKE_BUILD_TYPE_LOWERCASE} STREQUAL "debug")


# include common ApexParticles.cmake
INCLUDE(../common/ApexParticles.cmake)

# enable -fPIC so we can link static libs with the editor
SET_TARGET_PROPERTIES(APEX_Particles PROPERTIES POSITION_INDEPENDENT_CODE TRUE)
