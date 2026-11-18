/*
 * Xbox Game runtime Library
 *
 * Copyright 2026 Olivia Ryan
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

/* Built for upstream's MinGW headers: Wine's widl conventions (inline wrappers, const vtables). */
#ifndef __WINESRC__
#define __WINESRC__ 1
#endif
/* Like Wine: typed inline wrappers and const vtables in the widl headers. */
#define WIDL_C_INLINE_WRAPPERS
#define CONST_VTABLE
#define WIDL_EXPLICIT_AGGREGATE_RETURNS
#define COBJMACROS

#include <roapi.h>
#include <private/logging.h>
#include "wine_compat.h"
#include <winstring.h>

#include <xaccessibility.h>
#include <xappcapture.h>
#include <xasync.h>
#include <xasyncprovider.h>
#include <xdisplay.h>
#include <xerror.h>
#include <xgame.h>
#include <xgameactivation.h>
#include <xgameerr.h>
#include <xgameevent.h>
#include <xgameinvite.h>
#include <xgameprotocol.h>
#include <xgameruntimefeature.h>
#include <xgameruntimetypes.h>
#include <xgamesave.h>
#include <xgamestreaming.h>
#include <xgameui.h>
#include <xnetworking.h>
#include <xpackage.h>
#include <xpersistentlocalstorage.h>
#include <xspeechsynthesizer.h>
#include <xstore.h>
#include <xsystem.h>
#include <xtaskqueue.h>
#include <xuser.h>

/* April 2026 Release of GDK */
#define GDKC_VERSION 10002L
#define GAMING_SERVICES_VERSION 7822L

#define WIDL_using_Windows_Foundation
#define WIDL_using_Windows_Foundation_Collections
#include <windows.foundation.h>
#define WIDL_using_Windows_Globalization
#include <windows.globalization.h>
#define WIDL_using_Windows_System_Profile
#include <windows.system.profile.h>


extern IXAccessibilityImpl *x_accessibility_impl;
extern IXAppCaptureImpl *x_app_capture_impl;
extern IXAppCaptureMetadataImpl *x_app_capture_metadata_impl;
extern IXDisplayImpl *x_display_impl;
extern IXErrorImpl *x_error_impl;
extern IXGameImpl *x_game_impl;
extern IXGameActivationImpl *x_game_activation_impl;
extern IXGameEventImpl *x_game_event_impl;
extern IXGameInviteImpl *x_game_invite_impl;
extern IXGameProtocolImpl *x_game_protocol_impl;
extern IXGameRuntimeFeatureImpl *x_game_runtime_feature_impl;
extern IXGameSaveImpl *x_game_save_impl;
extern IXGameStreamingImpl *x_game_streaming_impl;
extern IXGameUiImpl *x_game_ui_impl;
extern IXLauncherImpl *x_launcher_impl;
extern IXNetworkingImpl *x_networking_impl;
extern IXPackageImpl *x_package_impl;
extern IXPersistentLocalStorageImpl *x_persistent_local_storage_impl;
extern IXStoreImpl *x_store_impl;
extern IXSystemImpl *x_system_impl;
extern IXSystemAnalyticsImpl *x_system_analytics_impl;
extern IXThreadingImpl *x_threading_impl;
extern IXUserImpl *x_user_impl;
extern IXUserDeviceImpl *x_user_device_impl;

HRESULT WINAPI QueryApiImpl( const GUID *classId, REFIID interfaceId, void **out );

BOOL gdkc_config_available(void);
BOOL gdkc_config_get_title_id( UINT32 *title_id );
BOOL gdkc_config_get_package_full_name( char *out, SIZE_T size );
BOOL gdkc_config_get_package_family_name( char *out, SIZE_T size );
BOOL gdkc_config_get_store_id( char *out, SIZE_T size );
BOOL gdkc_config_get_msa_app_id( char *out, SIZE_T size );
HRESULT gdkc_get_storage_path( char *out, SIZE_T size, SIZE_T *used );
BOOL gdkc_bridge_available(void);
HRESULT gdkc_bridge_post( const WCHAR *path, const char *body, char **response );

/* Installed add-ons (DLC) of the running game, see xodusaddons.c. */
#define GDKC_PACKAGE_ID_SIZE 33
#define GDKC_STORE_ID_SIZE 16

struct gdkc_addon
{
    char package_id[GDKC_PACKAGE_ID_SIZE]; /* what games see: 32 lower case hex digits */
    char content_id[40];                   /* the form the bridge uses */
    char store_id[GDKC_STORE_ID_SIZE];
    XVersion version;
    char *path;                            /* Windows path of the files */
    char *name;
    char *publisher;
};

struct gdkc_addons
{
    LONG ref;
    char generation[32];
    UINT32 count;
    struct gdkc_addon *items;
};

BOOL gdkc_normalize_package_id( const char *id, char out[GDKC_PACKAGE_ID_SIZE] );
BOOL gdkc_game_package_id( char out[GDKC_PACKAGE_ID_SIZE] );
HRESULT gdkc_addons_get( struct gdkc_addons **addons );
void gdkc_addons_release( struct gdkc_addons *addons );
const struct gdkc_addon *gdkc_addons_find( const struct gdkc_addons *addons, const char *id );
HRESULT gdkc_addon_license( const struct gdkc_addon *addon, BOOL *licensed );
void gdkc_addon_get_details( const struct gdkc_addon *addon, UINT32 index, UINT32 count, XPackageDetails *details );
HRESULT gdkc_addons_register_installed( XTaskQueueHandle queue, void *context, XPackageInstalledCallback *callback, XTaskQueueRegistrationToken *token );
BOOLEAN gdkc_addons_unregister_installed( XTaskQueueRegistrationToken token );
BOOL gdkc_config_get_version( XVersion *version );
BOOL gdkc_config_get_display_name( char *out, SIZE_T size );
BOOL gdkc_config_get_publisher_display_name( char *out, SIZE_T size );
