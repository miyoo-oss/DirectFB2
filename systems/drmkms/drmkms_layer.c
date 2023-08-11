/*
   This file is part of DirectFB.

   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 2.1 of the License, or (at your option) any later version.

   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with this library; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA
*/

#include <core/layers.h>
#include <direct/thread.h>

#include "drmkms_system.h"

/* Time out for flip assuming that almost everything can manage 5 fps */
static const int         fliptimeout = 1000000 / 5;

D_DEBUG_DOMAIN( DRMKMS_Layer, "DRMKMS/Layer", "DRM/KMS Layer" );

/**********************************************************************************************************************/

typedef struct {
     int                    primary_index;
     int                    plane_index;

     drmModePlane          *plane;
     uint32_t               colorkey_propid;
     uint32_t               zpos_propid;
     uint32_t               alpha_propid;

     int                    level;

     CoreLayerRegionConfig *config;
     bool                   muted;

     CoreSurface           *surface;
     int                    surfacebuffer_index;
     bool                   flip_pending;

     DirectMutex            lock;
     DirectWaitQueue        wq_event;
} DRMKMSLayerData;

static void
drmkms_page_flip_handler( int           fd,
                          unsigned int  frame,
                          unsigned int  sec,
                          unsigned int  usec,
                          void         *layer_data )
{
     DRMKMSLayerData *data = layer_data;

     D_DEBUG_AT( DRMKMS_Layer, "%s()\n", __FUNCTION__ );

     direct_mutex_lock( &data->lock );

     if (data->flip_pending) {
          dfb_surface_notify_display2( data->surface, data->surfacebuffer_index );

          dfb_surface_unref( data->surface );
     }

     data->flip_pending = false;

     direct_waitqueue_broadcast( &data->wq_event );

     direct_mutex_unlock( &data->lock );

     D_DEBUG_AT( DRMKMS_Layer, "%s() done\n", __FUNCTION__ );
}

static void *
drmkms_buffer_thread( DirectThread *thread,
                      void         *arg )
{
     DRMKMSData *drmkms = arg;

     D_DEBUG_AT( DRMKMS_Layer, "%s()\n", __FUNCTION__ );

     while (true)
          drmHandleEvent( drmkms->fd, &drmkms->event_context );

     return NULL;
}

/**********************************************************************************************************************/

static int
drmkmsPrimaryLayerDataSize( void )
{
     return sizeof(DRMKMSLayerData);
}

static DFBResult
drmkmsPrimaryInitLayer( CoreLayer                  *layer,
                        void                       *driver_data,
                        void                       *layer_data,
                        DFBDisplayLayerDescription *description,
                        DFBDisplayLayerConfig      *config,
                        DFBColorAdjustment         *adjustment )
{
     DRMKMSData       *drmkms = driver_data;
     DRMKMSDataShared *shared;
     DRMKMSLayerData  *data   = layer_data;

     D_DEBUG_AT( DRMKMS_Layer, "%s()\n", __FUNCTION__ );

     D_ASSERT( drmkms != NULL );
     D_ASSERT( drmkms->shared != NULL );
     D_ASSERT( data != NULL );

     shared = drmkms->shared;

     /* Initialize the layer data. */
     data->primary_index = shared->primary_index_count++;

     /* Set type and capabilities. */
     description->type             = DLTF_GRAPHICS;
     description->caps             = DLCAPS_SURFACE;
     description->surface_accessor = CSAID_LAYER0;

     /* Set name. */
     snprintf( description->name, DFB_DISPLAY_LAYER_DESC_NAME_LENGTH, "DRMKMS Primary Layer" );

     /* Fill out the default configuration. */
     config->flags       = DLCONF_WIDTH | DLCONF_HEIGHT | DLCONF_PIXELFORMAT | DLCONF_BUFFERMODE;
     config->width       = shared->mode[data->primary_index].hdisplay;
     config->height      = shared->mode[data->primary_index].vdisplay;
     config->pixelformat = dfb_config->mode.format ?: shared->primary_format;
     config->buffermode  = DLBM_FRONTONLY;

     direct_mutex_init( &data->lock );
     direct_waitqueue_init( &data->wq_event );

     drmkms->event_context.version           = DRM_EVENT_CONTEXT_VERSION;
     drmkms->event_context.vblank_handler    = drmkms_page_flip_handler;
     drmkms->event_context.page_flip_handler = drmkms_page_flip_handler;

     drmkms->thread = direct_thread_create( DTT_CRITICAL, drmkms_buffer_thread, drmkms, "DRMKMS Buffer" );

     return DFB_OK;
}

static DFBResult
drmkmsPrimaryTestRegion( CoreLayer                  *layer,
                         void                       *driver_data,
                         void                       *layer_data,
                         CoreLayerRegionConfig      *config,
                         CoreLayerRegionConfigFlags *ret_failed )
{

     DRMKMSData                 *drmkms = driver_data;
     DRMKMSDataShared           *shared;
     DRMKMSLayerData            *data   = layer_data;
     CoreLayerRegionConfigFlags  failed = CLRCF_NONE;

     D_DEBUG_AT( DRMKMS_Layer, "%s( %dx%d, %s )\n", __FUNCTION__,
                 config->width, config->height, dfb_pixelformat_name( config->format ) );

     D_ASSERT( drmkms != NULL );
     D_ASSERT( drmkms->shared != NULL );
     D_ASSERT( data != NULL );

     shared = drmkms->shared;

     if (shared->primary_dimension[data->primary_index].w > config->width ||
         shared->primary_dimension[data->primary_index].h > config->height) {
          failed = CLRCF_WIDTH | CLRCF_HEIGHT;

          D_DEBUG_AT( DRMKMS_Layer, "  -> rejection of layers smaller than the current primary layer\n" );
     }

     if (ret_failed)
          *ret_failed = failed;

     if (failed)
          return DFB_UNSUPPORTED;

     return DFB_OK;
}

static DFBResult
drmkmsPrimarySetRegion( CoreLayer                  *layer,
                        void                       *driver_data,
                        void                       *layer_data,
                        void                       *region_data,
                        CoreLayerRegionConfig      *config,
                        CoreLayerRegionConfigFlags  updated,
                        CoreSurface                *surface,
                        CorePalette                *palette,
                        CoreSurfaceBufferLock      *left_lock,
                        CoreSurfaceBufferLock      *right_lock )
{
     DFBResult         ret;
     int               err;
     DRMKMSData       *drmkms = driver_data;
     DRMKMSDataShared *shared;
     DRMKMSLayerData  *data   = layer_data;

     D_DEBUG_AT( DRMKMS_Layer, "%s()\n", __FUNCTION__ );

     D_ASSERT( drmkms != NULL );
     D_ASSERT( drmkms->shared != NULL );
     D_ASSERT( data != NULL );

     shared = drmkms->shared;

     if (updated & (CLRCF_WIDTH | CLRCF_HEIGHT | CLRCF_BUFFERMODE | CLRCF_SOURCE)) {
          int i;
          int index = data->primary_index;

          for (i = 0; i < drmkms->enabled_crtcs; i++) {
               if (shared->mirror_outputs)
                    index = i;

               err = drmModeSetCrtc( drmkms->fd, drmkms->encoder[index]->crtc_id,
                                     (uint32_t)(long) left_lock->handle, config->source.x, config->source.y,
                                     &drmkms->connector[index]->connector_id, 1, &shared->mode[index] );
               if (err) {
                    ret = errno2result( errno );
                    D_PERROR( "DRMKMS/Layer: "
                              "drmModeSetCrtc( crtc_id %u, fb_id %u, xy %d,%d, connector_id %u, mode %ux%u@%uHz )"
                              " failed at index %d!\n", drmkms->encoder[index]->crtc_id,
                              (uint32_t)(long) left_lock->handle, config->source.x, config->source.y,
                              drmkms->connector[index]->connector_id,
                              shared->mode[index].hdisplay, shared->mode[index].vdisplay, shared->mode[index].vrefresh,
                              index);
                    return ret;
               }

               if (!shared->mirror_outputs)
                    break;
          }

          shared->primary_dimension[data->primary_index] = surface->config.size;
          shared->primary_rect                           = config->source;
          shared->primary_fb                             = (uint32_t)(long) left_lock->handle;
     }

     return DFB_OK;
}

static DFBResult
drmkmsPrimaryUpdateFlipRegion( void                  *driver_data,
                               void                  *layer_data,
                               CoreSurface           *surface,
                               DFBSurfaceFlipFlags    flags,
                               CoreSurfaceBufferLock *left_lock,
                               bool                   flip )
{
     DFBResult         ret;
     int               err;
     DRMKMSData       *drmkms = driver_data;
     DRMKMSDataShared *shared;
     DRMKMSLayerData  *data   = layer_data;

     D_DEBUG_AT( DRMKMS_Layer, "%s()\n", __FUNCTION__ );

     D_ASSERT( drmkms != NULL );
     D_ASSERT( drmkms->shared != NULL );
     D_ASSERT( data != NULL );

     shared = drmkms->shared;

     direct_mutex_lock( &data->lock );

     while (data->flip_pending) {
          D_DEBUG_AT( DRMKMS_Layer, "  -> waiting for pending flip (previous)\n" );

          if (direct_waitqueue_wait_timeout( &data->wq_event, &data->lock, fliptimeout ) == DR_TIMEOUT) {
               D_WARN( "Timeout waiting for previous flip to complete\n" );
               break;
	  }
     }

     dfb_surface_ref( surface );

     data->surface             = surface;
     data->surfacebuffer_index = left_lock->buffer->index;
     data->flip_pending        = true;

     D_DEBUG_AT( DRMKMS_Layer, "  -> calling drmModePageFlip()\n" );

     err = drmModePageFlip( drmkms->fd, drmkms->encoder[data->primary_index]->crtc_id,
                            (uint32_t)(long) left_lock->handle, DRM_MODE_PAGE_FLIP_EVENT, data );
     if (err) {
          ret = errno2result( errno );
          D_PERROR( "DRMKMS/Layer: drmModePageFlip() failed!\n" );
          direct_mutex_unlock( &data->lock );
          return ret;
     }

     if (shared->mirror_outputs) {
          int i;

          for (i = 1; i < drmkms->enabled_crtcs; i++) {
               err = drmModePageFlip( drmkms->fd, drmkms->encoder[i]->crtc_id,
                                      (uint32_t)(long) left_lock->handle, DRM_MODE_PAGE_FLIP_ASYNC, NULL );
               if (err)
                    D_WARN( "page-flip failed for mirror on crtc id %u", drmkms->encoder[i]->crtc_id );
          }
     }

     if (flip)
          dfb_surface_flip( surface, false );

     if ((flags & DSFLIP_WAITFORSYNC) == DSFLIP_WAITFORSYNC) {
          while (data->flip_pending) {
               D_DEBUG_AT( DRMKMS_Layer, "  -> waiting for pending flip (WAITFORSYNC)\n" );

               if (direct_waitqueue_wait_timeout( &data->wq_event, &data->lock, fliptimeout ) == DR_TIMEOUT) {
                    D_WARN( "Timeout waiting for current flip to complete\n" );
                    break;
               }
          }
     }

     direct_mutex_unlock( &data->lock );

     return DFB_OK;
}

static DFBResult
drmkmsPrimaryFlipRegion( CoreLayer             *layer,
                         void                  *driver_data,
                         void                  *layer_data,
                         void                  *region_data,
                         CoreSurface           *surface,
                         DFBSurfaceFlipFlags    flags,
                         const DFBRegion       *left_update,
                         CoreSurfaceBufferLock *left_lock,
                         const DFBRegion       *right_update,
                         CoreSurfaceBufferLock *right_lock )
{
     return drmkmsPrimaryUpdateFlipRegion( driver_data, layer_data, surface, flags, left_lock, true );
}

static DFBResult
drmkmsPrimaryUpdateRegion( CoreLayer             *layer,
                           void                  *driver_data,
                           void                  *layer_data,
                           void                  *region_data,
                           CoreSurface           *surface,
                           const DFBRegion       *left_update,
                           CoreSurfaceBufferLock *left_lock,
                           const DFBRegion       *right_update,
                           CoreSurfaceBufferLock *right_lock )
{
     return drmkmsPrimaryUpdateFlipRegion( driver_data, layer_data, surface, DSFLIP_ONSYNC, left_lock, false );
}

static int
drmkmsPlaneLayerDataSize( void )
{
     return sizeof(DRMKMSLayerData);
}

static DFBResult
drmkmsPlaneInitLayer( CoreLayer                  *layer,
                      void                       *driver_data,
                      void                       *layer_data,
                      DFBDisplayLayerDescription *description,
                      DFBDisplayLayerConfig      *config,
                      DFBColorAdjustment         *adjustment )
{
     DRMKMSData              *drmkms = driver_data;
     DRMKMSDataShared        *shared;
     DRMKMSLayerData         *data   = layer_data;
     drmModeObjectProperties *props;
     drmModePropertyRes      *prop;
     int                      i;

     D_DEBUG_AT( DRMKMS_Layer, "%s()\n", __FUNCTION__ );

     D_ASSERT( drmkms != NULL );
     D_ASSERT( drmkms->shared != NULL );
     D_ASSERT( data != NULL );

     shared = drmkms->shared;

     /* Initialize the layer data. */
     data->plane_index = ++shared->plane_index_count;
     data->level       = data->plane_index;
     i                 = shared->layer_indices[data->plane_index];
     data->plane       = drmModeGetPlane( drmkms->fd, drmkms->plane_resources->planes[i] );

     D_DEBUG_AT( DRMKMS_Layer, "  -> getting plane with index %d\n", data->plane_index );
     D_DEBUG_AT( DRMKMS_Layer, "    => plane_id is %u\n", data->plane->plane_id );

     /* Set type and capabilities. */
     description->type             = DLTF_GRAPHICS;
     description->caps             = DLCAPS_SURFACE | DLCAPS_SCREEN_POSITION | DLCAPS_ALPHACHANNEL;
     description->surface_accessor = CSAID_LAYER0;

     /* Set name. */
     snprintf( description->name, DFB_DISPLAY_LAYER_DESC_NAME_LENGTH, "DRMKMS Plane Layer %d", data->plane_index );

     /* Fill out the default configuration. */
     config->flags       = DLCONF_WIDTH | DLCONF_HEIGHT | DLCONF_PIXELFORMAT | DLCONF_BUFFERMODE;
     config->width       = shared->mode[0].hdisplay;
     config->height      = shared->mode[0].vdisplay;
     config->pixelformat = dfb_config->mode.format ?: DSPF_ARGB;
     config->buffermode  = DLBM_FRONTONLY;

     props = drmModeObjectGetProperties( drmkms->fd, data->plane->plane_id, DRM_MODE_OBJECT_PLANE );
     if (props) {
          D_INFO( "DRMKMS/Layer: Supported properties for layer id %u\n", data->plane->plane_id );
          for (i = 0; i < props->count_props; i++) {
               prop = drmModeGetProperty( drmkms->fd, props->props[i] );

               if (!strcmp( prop->name, "colorkey" )) {
                    description->caps |= DLCAPS_SRC_COLORKEY;
                    data->colorkey_propid = prop->prop_id;
                    D_INFO( "     colorkey\n" );
               }
               else if (!strcmp( prop->name, "zpos" )) {
                    description->caps |= DLCAPS_LEVELS;
                    data->zpos_propid = prop->prop_id;
                    D_INFO( "     zpos\n" );

                    drmModeObjectSetProperty( drmkms->fd, data->plane->plane_id, DRM_MODE_OBJECT_PLANE,
                                              data->zpos_propid, data->level );
               }
               else if (!strcmp( prop->name, "alpha" )) {
                    description->caps |= DLCAPS_OPACITY;
                    data->alpha_propid = prop->prop_id;
                    D_INFO( "     alpha\n" );
               }

               drmModeFreeProperty( prop );
          }

          drmModeFreeObjectProperties( props );
     }

     return DFB_OK;
}

static DFBResult
drmkmsPlaneGetLevel( CoreLayer *layer,
                     void      *driver_data,
                     void      *layer_data,
                     int       *level )
{
     DRMKMSLayerData *data = layer_data;

     D_DEBUG_AT( DRMKMS_Layer, "%s()\n", __FUNCTION__ );

     if (level)
          *level = data->level;

     return DFB_OK;
}

static DFBResult
drmkmsPlaneSetLevel( CoreLayer *layer,
                     void      *driver_data,
                     void      *layer_data,
                     int        level )
{
     DFBResult         ret;
     int               err;
     DRMKMSData       *drmkms = driver_data;
     DRMKMSDataShared *shared;
     DRMKMSLayerData  *data   = layer_data;

     D_DEBUG_AT( DRMKMS_Layer, "%s()\n", __FUNCTION__ );

     D_ASSERT( drmkms != NULL );
     D_ASSERT( drmkms->shared != NULL );
     D_ASSERT( data != NULL );

     shared = drmkms->shared;

     if (!data->zpos_propid)
          return DFB_UNSUPPORTED;

     if (level < 1 || level > shared->plane_index_count)
          return DFB_INVARG;

     err = drmModeObjectSetProperty( drmkms->fd, data->plane->plane_id, DRM_MODE_OBJECT_PLANE,
                                     data->zpos_propid, level );
     if (err) {
          ret = errno2result( errno );
          D_PERROR( "DRMKMS/Layer: drmModeObjectSetProperty() failed setting zpos!\n" );
          return ret;
     }

     data->level = level;

     return DFB_OK;
}

static DFBResult
drmkmsPlaneTestRegion( CoreLayer                  *layer,
                       void                       *driver_data,
                       void                       *layer_data,
                       CoreLayerRegionConfig      *config,
                       CoreLayerRegionConfigFlags *ret_failed )
{
     DRMKMSLayerData            *data   = layer_data;
     CoreLayerRegionConfigFlags  failed = CLRCF_NONE;

     D_DEBUG_AT( DRMKMS_Layer, "%s()\n", __FUNCTION__ );

     if ((config->options & DLOP_SRC_COLORKEY) && !data->colorkey_propid)
          failed |= CLRCF_OPTIONS;

     if (ret_failed)
          *ret_failed = failed;

     if (failed)
          return DFB_UNSUPPORTED;

     return DFB_OK;
}

static DFBResult
drmkmsPlaneSetRegion( CoreLayer                  *layer,
                      void                       *driver_data,
                      void                       *layer_data,
                      void                       *region_data,
                      CoreLayerRegionConfig      *config,
                      CoreLayerRegionConfigFlags  updated,
                      CoreSurface                *surface,
                      CorePalette                *palette,
                      CoreSurfaceBufferLock      *left_lock,
                      CoreSurfaceBufferLock      *right_lock )
{
     DFBResult        ret;
     int              err;
     DRMKMSData      *drmkms = driver_data;
     DRMKMSLayerData *data   = layer_data;

     D_DEBUG_AT( DRMKMS_Layer, "%s()\n", __FUNCTION__ );

     if (( updated & (CLRCF_WIDTH | CLRCF_HEIGHT | CLRCF_BUFFERMODE | CLRCF_DEST | CLRCF_SOURCE)) ||
         ((updated & CLRCF_OPACITY) && data->muted && config->opacity)) {
          err = drmModeSetPlane( drmkms->fd, data->plane->plane_id, drmkms->encoder[0]->crtc_id,
                                 (uint32_t)(long) left_lock->handle, 0,
                                 config->dest.x, config->dest.y, config->dest.w, config->dest.h,
                                 config->source.x << 16, config->source.y << 16,
                                 config->source.w << 16, config->source.h << 16 );
          if (err) {
               ret = errno2result( errno );
               D_PERROR( "DRMKMS/Layer: "
                         "drmModeSetPlane( plane_id %u, fb_id %u, dest %4d,%4d-%4dx%4d, source %4d,%4d-%4dx%4d )"
                         " failed!\n", data->plane->plane_id, (u32)(long) left_lock->handle,
                         DFB_RECTANGLE_VALS( &config->dest ), DFB_RECTANGLE_VALS( &config->source ) );
               return ret;
          }

          data->config = config;
          data->muted  = false;
     }

     if ((updated & (CLRCF_SRCKEY | CLRCF_OPTIONS)) && data->colorkey_propid) {
          uint32_t colorkey = config->src_key.r << 16 | config->src_key.g << 8 | config->src_key.b;

          if (config->options & DLOP_SRC_COLORKEY)
               colorkey |= 0x01000000;

          err = drmModeObjectSetProperty( drmkms->fd, data->plane->plane_id, DRM_MODE_OBJECT_PLANE,
                                          data->colorkey_propid, colorkey );
          if (err) {
               ret = errno2result( errno );
               D_PERROR( "DRMKMS/Layer: drmModeObjectSetProperty() failed setting colorkey\n" );
               return ret;
          }
     }

     if (updated & CLRCF_OPACITY) {
          if (config->opacity == 0) {
               err = drmModeSetPlane( drmkms->fd, data->plane->plane_id, drmkms->encoder[0]->crtc_id, 0, 0,
                                      0, 0, 0, 0, 0, 0, 0, 0 );
               if (err) {
                    ret = errno2result( errno );
                    D_PERROR( "DRMKMS/Layer: drmModeSetPlane() failed disabling plane!\n" );
                    return DFB_FAILURE;
               }

               data->muted = true;
          }
          else if (data->alpha_propid) {
               err = drmModeObjectSetProperty( drmkms->fd, data->plane->plane_id, DRM_MODE_OBJECT_PLANE,
                                               data->alpha_propid, (65535 * config->opacity + 127) / 255 );
               if (err) {
                    ret = errno2result( errno );
                    D_PERROR( "DRMKMS/Layer: drmModeObjectSetProperty() failed setting alpha!\n" );
                    return ret;
               }
          }
     }

     return DFB_OK;
}

static DFBResult
drmkmsPlaneRemoveRegion( CoreLayer *layer,
                         void      *driver_data,
                         void      *layer_data,
                         void      *region_data )
{
     DFBResult        ret;
     int              err;
     DRMKMSData      *drmkms = driver_data;
     DRMKMSLayerData *data   = layer_data;

     D_DEBUG_AT( DRMKMS_Layer, "%s()\n", __FUNCTION__ );

     if (!data->muted) {
          err = drmModeSetPlane( drmkms->fd, data->plane->plane_id, drmkms->encoder[0]->crtc_id, 0, 0,
                                 0, 0, 0, 0, 0, 0, 0, 0 );
          if (err) {
               ret = errno2result( errno );
               D_PERROR( "DRMKMS/Layer: drmModeSetPlane() failed removing plane!\n" );
               return ret;
          }
     }

     return DFB_OK;
}

static DFBResult
drmkmsPlaneUpdateFlipRegion( void                  *driver_data,
                             void                  *layer_data,
                             CoreSurface           *surface,
                             DFBSurfaceFlipFlags    flags,
                             CoreSurfaceBufferLock *left_lock,
                             bool                   flip )
{
     DFBResult              ret;
     int                    err;
     DRMKMSData            *drmkms = driver_data;
     DRMKMSLayerData       *data   = layer_data;
     CoreLayerRegionConfig *config = data->config;
     drmVBlank              vbl;

     D_DEBUG_AT( DRMKMS_Layer, "%s()\n", __FUNCTION__ );

     if (!config->source.w || !config->source.h || !config->dest.w || !config->dest.h)
          return DFB_INVARG;

     direct_mutex_lock( &data->lock );

     while (data->flip_pending) {
          D_DEBUG_AT( DRMKMS_Layer, "  -> waiting for plane pending flip (previous)\n" );

          if (direct_waitqueue_wait_timeout( &data->wq_event, &data->lock, fliptimeout ) == DR_TIMEOUT) {
               D_WARN( "Timeout waiting for previous flip to complete\n" );
               break;
          }
     }

     dfb_surface_ref( surface );

     data->surface             = surface;
     data->surfacebuffer_index = left_lock->buffer->index;
     data->flip_pending        = true;

     if (!data->muted) {
          err = drmModeSetPlane( drmkms->fd, data->plane->plane_id, drmkms->encoder[0]->crtc_id,
                                 (uint32_t)(long) left_lock->handle, 0,
                                 config->dest.x, config->dest.y, config->dest.w, config->dest.h,
                                 config->source.x << 16, config->source.y << 16,
                                 config->source.w << 16, config->source.h << 16 );
          if (err) {
               ret = errno2result( errno );
               D_PERROR( "DRMKMS/Layer: Failed setting plane configuration!\n" );
               direct_mutex_unlock( &data->lock );
               return ret;
          }
     }

     if (flip)
          dfb_surface_flip( surface, false );

     vbl.request.type     = DRM_VBLANK_EVENT | DRM_VBLANK_RELATIVE;
     vbl.request.sequence = 1;
     vbl.request.signal   = (unsigned long) data;

     drmWaitVBlank( drmkms->fd, &vbl );

     if ((flags & DSFLIP_WAITFORSYNC) == DSFLIP_WAITFORSYNC) {
          while (data->flip_pending) {
               D_DEBUG_AT( DRMKMS_Layer, "  -> waiting for plane pending flip (WAITFORSYNC)\n" );

               if (direct_waitqueue_wait_timeout( &data->wq_event, &data->lock, fliptimeout ) == DR_TIMEOUT) {
                   D_WARN( "Timeout waiting for previous flip to complete\n" );
                   break;
               }
          }
     }

     direct_mutex_unlock( &data->lock );

     return DFB_OK;
}

static DFBResult
drmkmsPlaneFlipRegion( CoreLayer             *layer,
                       void                  *driver_data,
                       void                  *layer_data,
                       void                  *region_data,
                       CoreSurface           *surface,
                       DFBSurfaceFlipFlags    flags,
                       const DFBRegion       *left_update,
                       CoreSurfaceBufferLock *left_lock,
                       const DFBRegion       *right_update,
                       CoreSurfaceBufferLock *right_lock )
{
     return drmkmsPlaneUpdateFlipRegion( driver_data, layer_data, surface, flags, left_lock, true );
}

static DFBResult
drmkmsPlaneUpdateRegion( CoreLayer             *layer,
                         void                  *driver_data,
                         void                  *layer_data,
                         void                  *region_data,
                         CoreSurface           *surface,
                         const DFBRegion       *left_update,
                         CoreSurfaceBufferLock *left_lock,
                         const DFBRegion       *right_update,
                         CoreSurfaceBufferLock *right_lock )
{
     return drmkmsPlaneUpdateFlipRegion( driver_data, layer_data, surface, DSFLIP_ONSYNC, left_lock, false );
}

const DisplayLayerFuncs drmkmsPrimaryLayerFuncs = {
     .LayerDataSize = drmkmsPrimaryLayerDataSize,
     .InitLayer     = drmkmsPrimaryInitLayer,
     .TestRegion    = drmkmsPrimaryTestRegion,
     .SetRegion     = drmkmsPrimarySetRegion,
     .FlipRegion    = drmkmsPrimaryFlipRegion,
     .UpdateRegion  = drmkmsPrimaryUpdateRegion
};

const DisplayLayerFuncs drmkmsPlaneLayerFuncs = {
     .LayerDataSize = drmkmsPlaneLayerDataSize,
     .InitLayer     = drmkmsPlaneInitLayer,
     .GetLevel      = drmkmsPlaneGetLevel,
     .SetLevel      = drmkmsPlaneSetLevel,
     .TestRegion    = drmkmsPlaneTestRegion,
     .SetRegion     = drmkmsPlaneSetRegion,
     .RemoveRegion  = drmkmsPlaneRemoveRegion,
     .FlipRegion    = drmkmsPlaneFlipRegion,
     .UpdateRegion  = drmkmsPlaneUpdateRegion
};
