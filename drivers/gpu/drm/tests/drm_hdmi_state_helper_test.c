// SPDX-License-Identifier: GPL-2.0

/*
 * Kunit test for drm_hdmi_state_helper functions
 */

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_state_helper.h>
#include <drm/drm_atomic_uapi.h>
#include <drm/drm_drv.h>
#include <drm/drm_edid.h>
#include <drm/drm_connector.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_kunit_helpers.h>
#include <drm/drm_managed.h>
#include <drm/drm_modeset_helper_vtables.h>
#include <drm/drm_print.h>
#include <drm/drm_probe_helper.h>

#include <drm/display/drm_hdmi_helper.h>
#include <drm/display/drm_hdmi_state_helper.h>

#include "../drm_crtc_internal.h"

#include <kunit/test.h>

#include "drm_kunit_edid.h"

struct drm_atomic_helper_connector_hdmi_priv {
	struct drm_device drm;
	struct drm_plane *plane;
	struct drm_crtc *crtc;
	struct drm_encoder encoder;
	struct drm_connector connector;
	struct drm_connector_hdmi_funcs hdmi_funcs;

	const void *current_edid;
	size_t current_edid_len;

	int hdmi_update_failures;
};

#define connector_to_priv(c) \
	container_of_const(c, struct drm_atomic_helper_connector_hdmi_priv, connector)

#define encoder_to_priv(e) \
	container_of_const(e, struct drm_atomic_helper_connector_hdmi_priv, encoder)

static struct drm_display_mode *find_preferred_mode(struct drm_connector *connector)
{
	struct drm_device *drm = connector->dev;
	struct drm_display_mode *mode, *preferred;

	mutex_lock(&drm->mode_config.mutex);
	preferred = list_first_entry_or_null(&connector->modes, struct drm_display_mode, head);
	list_for_each_entry(mode, &connector->modes, head)
		if (mode->type & DRM_MODE_TYPE_PREFERRED)
			preferred = mode;
	mutex_unlock(&drm->mode_config.mutex);

	return preferred;
}

static struct drm_display_mode *find_420_only_mode(struct drm_connector *connector)
{
	struct drm_device *drm = connector->dev;
	struct drm_display_mode *mode;

	mutex_lock(&drm->mode_config.mutex);
	list_for_each_entry(mode, &connector->modes, head) {
		if (drm_mode_is_420_only(&connector->display_info, mode)) {
			mutex_unlock(&drm->mode_config.mutex);
			return mode;
		}
	}
	mutex_unlock(&drm->mode_config.mutex);

	return NULL;
}

static struct drm_display_mode *find_420_also_mode(struct drm_connector *connector)
{
	struct drm_device *drm = connector->dev;
	struct drm_display_mode *mode;

	mutex_lock(&drm->mode_config.mutex);
	list_for_each_entry(mode, &connector->modes, head) {
		if (drm_mode_is_420_also(&connector->display_info, mode)) {
			mutex_unlock(&drm->mode_config.mutex);
			return mode;
		}
	}
	mutex_unlock(&drm->mode_config.mutex);

	return NULL;
}

static int set_connector_edid(struct kunit *test, struct drm_connector *connector,
			      const void *edid, size_t edid_len)
{
	struct drm_atomic_helper_connector_hdmi_priv *priv =
		connector_to_priv(connector);
	struct drm_device *drm = connector->dev;
	int ret;

	priv->current_edid = edid;
	priv->current_edid_len = edid_len;

	mutex_lock(&drm->mode_config.mutex);
	ret = connector->funcs->fill_modes(connector, 4096, 4096);
	mutex_unlock(&drm->mode_config.mutex);

	return ret;
}

static int accept_infoframe_clear_infoframe(struct drm_connector *connector)
{
	return 0;
}

static int accept_infoframe_write_infoframe(struct drm_connector *connector,
					    const u8 *buffer, size_t len)
{
	return 0;
}

static const struct drm_connector_hdmi_funcs dummy_connector_hdmi_funcs = {
	.vendor = "Vendor",
	.product = "Product",
	.supported_hdmi_ver = HDMI_VERSION_1_4,
	.supported_formats = BIT(DRM_OUTPUT_COLOR_FORMAT_RGB444),
	.max_bpc = 8,
	.avi = {
		.clear_infoframe = accept_infoframe_clear_infoframe,
		.write_infoframe = accept_infoframe_write_infoframe,
	},
	.hdmi = {
		.clear_infoframe = accept_infoframe_clear_infoframe,
		.write_infoframe = accept_infoframe_write_infoframe,
	},
};

static enum drm_mode_status
reject_connector_tmds_char_rate_valid(const struct drm_connector *connector,
				      const struct drm_display_mode *mode,
				      unsigned long long tmds_rate)
{
	return MODE_BAD;
}

static const struct drm_connector_hdmi_funcs reject_connector_hdmi_funcs = {
	.vendor = "Vendor",
	.product = "Product",
	.supported_hdmi_ver = HDMI_VERSION_1_4,
	.supported_formats = BIT(DRM_OUTPUT_COLOR_FORMAT_RGB444),
	.max_bpc = 8,
	.tmds_char_rate_valid	= reject_connector_tmds_char_rate_valid,
	.avi = {
		.clear_infoframe = accept_infoframe_clear_infoframe,
		.write_infoframe = accept_infoframe_write_infoframe,
	},
	.hdmi = {
		.clear_infoframe = accept_infoframe_clear_infoframe,
		.write_infoframe = accept_infoframe_write_infoframe,
	},
};

static enum drm_mode_status
reject_100mhz_connector_tmds_char_rate_valid(const struct drm_connector *connector,
					     const struct drm_display_mode *mode,
					     unsigned long long tmds_rate)
{
	return (tmds_rate > 100ULL * 1000 * 1000) ? MODE_BAD : MODE_OK;
}

static const struct drm_connector_hdmi_funcs reject_100mhz_connector_hdmi_funcs = {
	.vendor = "Vendor",
	.product = "Product",
	.supported_hdmi_ver = HDMI_VERSION_1_4,
	.supported_formats = BIT(DRM_OUTPUT_COLOR_FORMAT_RGB444),
	.max_bpc = 8,
	.tmds_char_rate_valid	= reject_100mhz_connector_tmds_char_rate_valid,
	.avi = {
		.clear_infoframe = accept_infoframe_clear_infoframe,
		.write_infoframe = accept_infoframe_write_infoframe,
	},
	.hdmi = {
		.clear_infoframe = accept_infoframe_clear_infoframe,
		.write_infoframe = accept_infoframe_write_infoframe,
	},
};

static enum drm_mode_status
accept_any_connector_tmds_char_rate_valid(const struct drm_connector *connector,
					  const struct drm_display_mode *mode,
					  unsigned long long tmds_rate)
{
	return MODE_OK;
}

static const struct drm_connector_hdmi_funcs reject_over_165mhz_connector_hdmi_funcs = {
	.vendor = "Vendor",
	.product = "Product",
	.supported_hdmi_ver = HDMI_VERSION_1_3,
	.supported_tmds_char_rate = HDMI_1_0_TMDS_CHAR_RATE_MAX_HZ,
	.supported_formats = BIT(DRM_OUTPUT_COLOR_FORMAT_RGB444),
	.max_bpc = 8,
	.tmds_char_rate_valid = accept_any_connector_tmds_char_rate_valid,
	.avi = {
		.clear_infoframe = accept_infoframe_clear_infoframe,
		.write_infoframe = accept_infoframe_write_infoframe,
	},
	.hdmi = {
		.clear_infoframe = accept_infoframe_clear_infoframe,
		.write_infoframe = accept_infoframe_write_infoframe,
	},
};

static int accept_scrambler_enable(struct drm_connector *connector)
{
	return 0;
}

static int accept_scrambler_disable(struct drm_connector *connector)
{
	return 0;
}

static const struct drm_connector_hdmi_funcs scrambler_connector_hdmi_funcs = {
	.vendor = "Vendor",
	.product = "Product",
	.supported_hdmi_ver = HDMI_VERSION_2_0,
	.supported_formats = BIT(DRM_OUTPUT_COLOR_FORMAT_RGB444),
	.max_bpc = 8,
	.scrambler_enable	 = accept_scrambler_enable,
	.scrambler_disable	 = accept_scrambler_disable,
	.avi = {
		.clear_infoframe = accept_infoframe_clear_infoframe,
		.write_infoframe = accept_infoframe_write_infoframe,
	},
	.hdmi = {
		.clear_infoframe = accept_infoframe_clear_infoframe,
		.write_infoframe = accept_infoframe_write_infoframe,
	},
};

static int accept_frl_configure(struct drm_connector *connector,
				u8 rate_per_lane, u8 lanes)
{
	return 0;
}

static int accept_frl_set_ltp(struct drm_connector *connector,
			      u8 ln0, u8 ln1, u8 ln2, u8 ln3)
{
	return 0;
}

static int accept_frl_tx_start(struct drm_connector *connector)
{
	return 0;
}

static int accept_frl_tx_stop(struct drm_connector *connector)
{
	return 0;
}

static int accept_frl_fallback_tmds(struct drm_connector *connector)
{
	return 0;
}

static enum drm_mode_status
accept_48gbps_connector_frl_rate_valid(const struct drm_connector *connector,
				       const struct drm_display_mode *mode,
				       unsigned int min_frl_rate,
				       unsigned int max_frl_rate,
				       unsigned int *pref_frl_rate)
{
	if (min_frl_rate > 48)
		return MODE_BAD;

	*pref_frl_rate = 48;

	return MODE_OK;
}

static const struct drm_connector_hdmi_funcs accept_frl_connector_hdmi_funcs = {
	.vendor = "Vendor",
	.product = "Product",
	.supported_hdmi_ver = HDMI_VERSION_2_1,
	.supported_formats = BIT(DRM_OUTPUT_COLOR_FORMAT_RGB444),
	.max_bpc = 8,
	.frl_configure = accept_frl_configure,
	.frl_set_ltp = accept_frl_set_ltp,
	.frl_tx_start = accept_frl_tx_start,
	.frl_tx_stop = accept_frl_tx_stop,
	.frl_fallback_tmds = accept_frl_fallback_tmds,
	.frl_rate_valid = accept_48gbps_connector_frl_rate_valid,
	.scrambler_enable	 = accept_scrambler_enable,
	.scrambler_disable	 = accept_scrambler_disable,
	.avi = {
		.clear_infoframe = accept_infoframe_clear_infoframe,
		.write_infoframe = accept_infoframe_write_infoframe,
	},
	.hdmi = {
		.clear_infoframe = accept_infoframe_clear_infoframe,
		.write_infoframe = accept_infoframe_write_infoframe,
	},
};

static enum drm_mode_status
reject_connector_frl_rate_valid(const struct drm_connector *connector,
				const struct drm_display_mode *mode,
				unsigned int min_frl_rate,
				unsigned int max_frl_rate,
				unsigned int *pref_frl_rate)
{
	return MODE_BAD;
}

static const struct drm_connector_hdmi_funcs reject_frl_connector_hdmi_funcs = {
	.vendor = "Vendor",
	.product = "Product",
	.supported_hdmi_ver = HDMI_VERSION_2_1,
	.supported_formats = BIT(DRM_OUTPUT_COLOR_FORMAT_RGB444),
	.max_bpc = 8,
	.frl_configure = accept_frl_configure,
	.frl_set_ltp = accept_frl_set_ltp,
	.frl_tx_start = accept_frl_tx_start,
	.frl_tx_stop = accept_frl_tx_stop,
	.frl_fallback_tmds = accept_frl_fallback_tmds,
	.frl_rate_valid = reject_connector_frl_rate_valid,
	.scrambler_enable	 = accept_scrambler_enable,
	.scrambler_disable	 = accept_scrambler_disable,
	.avi = {
		.clear_infoframe = accept_infoframe_clear_infoframe,
		.write_infoframe = accept_infoframe_write_infoframe,
	},
	.hdmi = {
		.clear_infoframe = accept_infoframe_clear_infoframe,
		.write_infoframe = accept_infoframe_write_infoframe,
	},
};

static int dummy_connector_get_modes(struct drm_connector *connector)
{
	struct drm_atomic_helper_connector_hdmi_priv *priv =
		connector_to_priv(connector);
	const struct drm_edid *edid;
	unsigned int num_modes;

	edid = drm_edid_alloc(priv->current_edid, priv->current_edid_len);
	if (!edid)
		return -EINVAL;

	drm_edid_connector_update(connector, edid);
	num_modes = drm_edid_connector_add_modes(connector);

	drm_edid_free(edid);

	return num_modes;
}

static const struct drm_connector_helper_funcs dummy_connector_helper_funcs = {
	.atomic_check	= drm_atomic_helper_connector_hdmi_check,
	.get_modes	= dummy_connector_get_modes,
	.mode_valid	= drm_hdmi_connector_mode_valid,
};

static void dummy_hdmi_connector_reset(struct drm_connector *connector)
{
	drm_atomic_helper_connector_reset(connector);
	__drm_atomic_helper_connector_hdmi_state_init(connector, connector->state);
}

static const struct drm_connector_funcs dummy_connector_funcs = {
	.atomic_destroy_state	= drm_atomic_helper_connector_destroy_state,
	.atomic_duplicate_state	= drm_atomic_helper_connector_duplicate_state,
	.fill_modes		= drm_helper_probe_single_connector_modes,
	.reset			= dummy_hdmi_connector_reset,
};

static void test_encoder_atomic_enable(struct drm_encoder *encoder,
			      struct drm_atomic_commit *state)
{
	struct drm_atomic_helper_connector_hdmi_priv *priv =
		encoder_to_priv(encoder);
	int ret;

	ret = drm_atomic_helper_connector_hdmi_update_infoframes(&priv->connector, state);
	if (ret)
		priv->hdmi_update_failures++;
}

static const struct drm_encoder_helper_funcs test_encoder_helper_funcs = {
	.atomic_enable = test_encoder_atomic_enable,
};

static
struct drm_atomic_helper_connector_hdmi_priv *
__connector_hdmi_init(struct kunit *test,
		      const struct drm_connector_hdmi_funcs *hdmi_funcs,
		      const void *edid_data, size_t edid_len)
{
	struct drm_atomic_helper_connector_hdmi_priv *priv;
	struct drm_connector *conn;
	struct drm_encoder *enc;
	struct drm_device *drm;
	struct device *dev;
	int ret;

	dev = drm_kunit_helper_alloc_device(test);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, dev);

	priv = drm_kunit_helper_alloc_drm_device(test, dev,
						 struct drm_atomic_helper_connector_hdmi_priv, drm,
						 DRIVER_MODESET | DRIVER_ATOMIC);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, priv);
	test->priv = priv;

	drm = &priv->drm;
	priv->plane = drm_kunit_helper_create_primary_plane(test, drm,
							    NULL,
							    NULL,
							    NULL, 0,
							    NULL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, priv->plane);

	priv->crtc = drm_kunit_helper_create_crtc(test, drm,
						  priv->plane, NULL,
						  NULL,
						  NULL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, priv->crtc);

	enc = &priv->encoder;
	ret = drmm_encoder_init(drm, enc, NULL, DRM_MODE_ENCODER_TMDS, NULL);
	KUNIT_ASSERT_EQ(test, ret, 0);

	enc->possible_crtcs = drm_crtc_mask(priv->crtc);

	conn = &priv->connector;
	conn->ycbcr_420_allowed = !!(hdmi_funcs->supported_formats &
				     BIT(DRM_OUTPUT_COLOR_FORMAT_YCBCR420));

	priv->hdmi_funcs = *hdmi_funcs;
	ret = drmm_connector_hdmi_init(drm, conn,
				       &dummy_connector_funcs,
				       &priv->hdmi_funcs,
				       DRM_MODE_CONNECTOR_HDMIA,
				       NULL);
	KUNIT_ASSERT_EQ(test, ret, 0);

	drm_connector_helper_add(conn, &dummy_connector_helper_funcs);
	drm_connector_attach_encoder(conn, enc);

	drm_mode_config_reset(drm);

	if (edid_data && edid_len) {
		ret = set_connector_edid(test, &priv->connector, edid_data, edid_len);
		KUNIT_ASSERT_GT(test, ret, 0);
	}

	return priv;
}

#define drm_kunit_helper_connector_hdmi_init_with_edid_funcs(test, funcs, edid)	\
	__connector_hdmi_init(test, funcs, edid, ARRAY_SIZE(edid))

static
struct drm_atomic_helper_connector_hdmi_priv *
drm_kunit_helper_connector_hdmi_init(struct kunit *test)
{
	return drm_kunit_helper_connector_hdmi_init_with_edid_funcs(test,
				&dummy_connector_hdmi_funcs,
				test_edid_hdmi_1080p_rgb_max_200mhz);
}

/*
 * Test that if we change the RGB quantization property to a different
 * value, we trigger a mode change on the connector's CRTC, which will
 * in turn disable/enable the connector.
 */
static void drm_test_check_broadcast_rgb_crtc_mode_changed(struct kunit *test)
{
	struct drm_atomic_helper_connector_hdmi_priv *priv;
	struct drm_modeset_acquire_ctx ctx;
	struct drm_connector_state *old_conn_state;
	struct drm_connector_state *new_conn_state;
	struct drm_crtc_state *crtc_state;
	struct drm_atomic_commit *state;
	struct drm_display_mode *preferred;
	struct drm_connector *conn;
	struct drm_device *drm;
	struct drm_crtc *crtc;
	int ret;

	priv = drm_kunit_helper_connector_hdmi_init(test);
	KUNIT_ASSERT_NOT_NULL(test, priv);

	drm = &priv->drm;
	crtc = priv->crtc;
	conn = &priv->connector;

	preferred = find_preferred_mode(conn);
	KUNIT_ASSERT_NOT_NULL(test, preferred);

	drm_modeset_acquire_init(&ctx, 0);

retry_conn_enable:
	ret = drm_kunit_helper_enable_crtc_connector(test, drm,
						     crtc, conn,
						     preferred,
						     &ctx);
	if (ret == -EDEADLK) {
		ret = drm_modeset_backoff(&ctx);
		if (!ret)
			goto retry_conn_enable;
	}
	KUNIT_ASSERT_EQ(test, ret, 0);

	state = drm_kunit_helper_atomic_state_alloc(test, drm, &ctx);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, state);

	new_conn_state = drm_atomic_get_connector_state(state, conn);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, new_conn_state);

	old_conn_state = drm_atomic_get_old_connector_state(state, conn);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, old_conn_state);

	new_conn_state->hdmi.broadcast_rgb = DRM_HDMI_BROADCAST_RGB_FULL;

	KUNIT_ASSERT_NE(test,
			old_conn_state->hdmi.broadcast_rgb,
			new_conn_state->hdmi.broadcast_rgb);

	ret = drm_atomic_check_only(state);
	KUNIT_ASSERT_EQ(test, ret, 0);

	new_conn_state = drm_atomic_get_new_connector_state(state, conn);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, new_conn_state);
	KUNIT_EXPECT_EQ(test, new_conn_state->hdmi.broadcast_rgb, DRM_HDMI_BROADCAST_RGB_FULL);

	crtc_state = drm_atomic_get_new_crtc_state(state, crtc);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, crtc_state);
	KUNIT_EXPECT_TRUE(test, crtc_state->mode_changed);

	drm_modeset_drop_locks(&ctx);
	drm_modeset_acquire_fini(&ctx);
}

/*
 * Test that if we set the RGB quantization property to the same value,
 * we don't trigger a mode change on the connector's CRTC and leave the
 * connector unaffected.
 */
static void drm_test_check_broadcast_rgb_crtc_mode_not_changed(struct kunit *test)
{
	struct drm_atomic_helper_connector_hdmi_priv *priv;
	struct drm_modeset_acquire_ctx ctx;
	struct drm_connector_state *old_conn_state;
	struct drm_connector_state *new_conn_state;
	struct drm_crtc_state *crtc_state;
	struct drm_atomic_commit *state;
	struct drm_display_mode *preferred;
	struct drm_connector *conn;
	struct drm_device *drm;
	struct drm_crtc *crtc;
	int ret;

	priv = drm_kunit_helper_connector_hdmi_init(test);
	KUNIT_ASSERT_NOT_NULL(test, priv);

	drm = &priv->drm;
	crtc = priv->crtc;
	conn = &priv->connector;

	preferred = find_preferred_mode(conn);
	KUNIT_ASSERT_NOT_NULL(test, preferred);

	drm_modeset_acquire_init(&ctx, 0);

retry_conn_enable:
	ret = drm_kunit_helper_enable_crtc_connector(test, drm,
						     crtc, conn,
						     preferred,
						     &ctx);
	if (ret == -EDEADLK) {
		ret = drm_modeset_backoff(&ctx);
		if (!ret)
			goto retry_conn_enable;
	}
	KUNIT_ASSERT_EQ(test, ret, 0);

	state = drm_kunit_helper_atomic_state_alloc(test, drm, &ctx);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, state);

	new_conn_state = drm_atomic_get_connector_state(state, conn);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, new_conn_state);

	old_conn_state = drm_atomic_get_old_connector_state(state, conn);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, old_conn_state);

	new_conn_state->hdmi.broadcast_rgb = old_conn_state->hdmi.broadcast_rgb;

	ret = drm_atomic_check_only(state);
	KUNIT_ASSERT_EQ(test, ret, 0);

	old_conn_state = drm_atomic_get_old_connector_state(state, conn);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, old_conn_state);

	new_conn_state = drm_atomic_get_new_connector_state(state, conn);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, new_conn_state);

	KUNIT_EXPECT_EQ(test,
			old_conn_state->hdmi.broadcast_rgb,
			new_conn_state->hdmi.broadcast_rgb);

	crtc_state = drm_atomic_get_new_crtc_state(state, crtc);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, crtc_state);
	KUNIT_EXPECT_FALSE(test, crtc_state->mode_changed);

	drm_modeset_drop_locks(&ctx);
	drm_modeset_acquire_fini(&ctx);
}

/*
 * Test that for an HDMI connector, with an HDMI monitor, if the
 * Broadcast RGB property is set to auto with a mode that isn't the
 * VIC-1 mode, we will get a limited RGB Quantization Range.
 */
static void drm_test_check_broadcast_rgb_auto_cea_mode(struct kunit *test)
{
	struct drm_atomic_helper_connector_hdmi_priv *priv;
	struct drm_modeset_acquire_ctx ctx;
	struct drm_connector_state *conn_state;
	struct drm_atomic_commit *state;
	struct drm_display_mode *preferred;
	struct drm_connector *conn;
	struct drm_device *drm;
	struct drm_crtc *crtc;
	int ret;

	priv = drm_kunit_helper_connector_hdmi_init(test);
	KUNIT_ASSERT_NOT_NULL(test, priv);

	drm = &priv->drm;
	crtc = priv->crtc;
	conn = &priv->connector;
	KUNIT_ASSERT_TRUE(test, conn->display_info.is_hdmi);

	preferred = find_preferred_mode(conn);
	KUNIT_ASSERT_NOT_NULL(test, preferred);
	KUNIT_ASSERT_NE(test, drm_match_cea_mode(preferred), 1);

	drm_modeset_acquire_init(&ctx, 0);

retry_conn_enable:
	ret = drm_kunit_helper_enable_crtc_connector(test, drm,
						     crtc, conn,
						     preferred,
						     &ctx);
	if (ret == -EDEADLK) {
		ret = drm_modeset_backoff(&ctx);
		if (!ret)
			goto retry_conn_enable;
	}
	KUNIT_ASSERT_EQ(test, ret, 0);

	state = drm_kunit_helper_atomic_state_alloc(test, drm, &ctx);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, state);

	conn_state = drm_atomic_get_connector_state(state, conn);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, conn_state);

	KUNIT_ASSERT_EQ(test,
			conn_state->hdmi.broadcast_rgb,
			DRM_HDMI_BROADCAST_RGB_AUTO);

	ret = drm_atomic_check_only(state);
	KUNIT_ASSERT_EQ(test, ret, 0);

	conn_state = drm_atomic_get_new_connector_state(state, conn);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, conn_state);

	KUNIT_EXPECT_TRUE(test, conn_state->hdmi.is_limited_range);

	drm_modeset_drop_locks(&ctx);
	drm_modeset_acquire_fini(&ctx);
}

/*
 * Test that for an HDMI connector, with an HDMI monitor, if the
 * Broadcast RGB property is set to auto with a VIC-1 mode, we will get
 * a full RGB Quantization Range.
 */
static void drm_test_check_broadcast_rgb_auto_cea_mode_vic_1(struct kunit *test)
{
	struct drm_atomic_helper_connector_hdmi_priv *priv;
	struct drm_modeset_acquire_ctx ctx;
	struct drm_connector_state *conn_state;
	struct drm_atomic_commit *state;
	struct drm_display_mode *mode;
	struct drm_connector *conn;
	struct drm_device *drm;
	struct drm_crtc *crtc;
	int ret;

	priv = drm_kunit_helper_connector_hdmi_init(test);
	KUNIT_ASSERT_NOT_NULL(test, priv);

	drm = &priv->drm;
	conn = &priv->connector;
	KUNIT_ASSERT_TRUE(test, conn->display_info.is_hdmi);

	drm_modeset_acquire_init(&ctx, 0);

	mode = drm_kunit_display_mode_from_cea_vic(test, drm, 1);
	KUNIT_ASSERT_NOT_NULL(test, mode);

	crtc = priv->crtc;

retry_conn_enable:
	ret = drm_kunit_helper_enable_crtc_connector(test, drm,
						     crtc, conn,
						     mode,
						     &ctx);
	if (ret == -EDEADLK) {
		ret = drm_modeset_backoff(&ctx);
		if (!ret)
			goto retry_conn_enable;
	}
	KUNIT_ASSERT_EQ(test, ret, 0);

	state = drm_kunit_helper_atomic_state_alloc(test, drm, &ctx);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, state);

	conn_state = drm_atomic_get_connector_state(state, conn);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, conn_state);

	KUNIT_ASSERT_EQ(test,
			conn_state->hdmi.broadcast_rgb,
			DRM_HDMI_BROADCAST_RGB_AUTO);

	ret = drm_atomic_check_only(state);
	KUNIT_ASSERT_EQ(test, ret, 0);

	conn_state = drm_atomic_get_new_connector_state(state, conn);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, conn_state);

	KUNIT_EXPECT_FALSE(test, conn_state->hdmi.is_limited_range);

	drm_modeset_drop_locks(&ctx);
	drm_modeset_acquire_fini(&ctx);
}

/*
 * Test that for an HDMI connector, with an HDMI monitor, if the
 * Broadcast RGB property is set to full with a mode that isn't the
 * VIC-1 mode, we will get a full RGB Quantization Range.
 */
static void drm_test_check_broadcast_rgb_full_cea_mode(struct kunit *test)
{
	struct drm_atomic_helper_connector_hdmi_priv *priv;
	struct drm_modeset_acquire_ctx ctx;
	struct drm_connector_state *conn_state;
	struct drm_atomic_commit *state;
	struct drm_display_mode *preferred;
	struct drm_connector *conn;
	struct drm_device *drm;
	struct drm_crtc *crtc;
	int ret;

	priv = drm_kunit_helper_connector_hdmi_init(test);
	KUNIT_ASSERT_NOT_NULL(test, priv);

	drm = &priv->drm;
	crtc = priv->crtc;
	conn = &priv->connector;
	KUNIT_ASSERT_TRUE(test, conn->display_info.is_hdmi);

	preferred = find_preferred_mode(conn);
	KUNIT_ASSERT_NOT_NULL(test, preferred);
	KUNIT_ASSERT_NE(test, drm_match_cea_mode(preferred), 1);

	drm_modeset_acquire_init(&ctx, 0);

retry_conn_enable:
	ret = drm_kunit_helper_enable_crtc_connector(test, drm,
						     crtc, conn,
						     preferred,
						     &ctx);
	if (ret == -EDEADLK) {
		ret = drm_modeset_backoff(&ctx);
		if (!ret)
			goto retry_conn_enable;
	}
	KUNIT_ASSERT_EQ(test, ret, 0);

	state = drm_kunit_helper_atomic_state_alloc(test, drm, &ctx);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, state);

	conn_state = drm_atomic_get_connector_state(state, conn);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, conn_state);

	conn_state->hdmi.broadcast_rgb = DRM_HDMI_BROADCAST_RGB_FULL;

	ret = drm_atomic_check_only(state);
	KUNIT_ASSERT_EQ(test, ret, 0);

	conn_state = drm_atomic_get_new_connector_state(state, conn);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, conn_state);

	KUNIT_ASSERT_EQ(test,
			conn_state->hdmi.broadcast_rgb,
			DRM_HDMI_BROADCAST_RGB_FULL);

	KUNIT_EXPECT_FALSE(test, conn_state->hdmi.is_limited_range);

	drm_modeset_drop_locks(&ctx);
	drm_modeset_acquire_fini(&ctx);
}

/*
 * Test that for an HDMI connector, with an HDMI monitor, if the
 * Broadcast RGB property is set to full with a VIC-1 mode, we will get
 * a full RGB Quantization Range.
 */
static void drm_test_check_broadcast_rgb_full_cea_mode_vic_1(struct kunit *test)
{
	struct drm_atomic_helper_connector_hdmi_priv *priv;
	struct drm_modeset_acquire_ctx ctx;
	struct drm_connector_state *conn_state;
	struct drm_atomic_commit *state;
	struct drm_display_mode *mode;
	struct drm_connector *conn;
	struct drm_device *drm;
	struct drm_crtc *crtc;
	int ret;

	priv = drm_kunit_helper_connector_hdmi_init(test);
	KUNIT_ASSERT_NOT_NULL(test, priv);

	drm = &priv->drm;
	conn = &priv->connector;
	KUNIT_ASSERT_TRUE(test, conn->display_info.is_hdmi);

	drm_modeset_acquire_init(&ctx, 0);

	mode = drm_kunit_display_mode_from_cea_vic(test, drm, 1);
	KUNIT_ASSERT_NOT_NULL(test, mode);

	crtc = priv->crtc;

retry_conn_enable:
	ret = drm_kunit_helper_enable_crtc_connector(test, drm,
						     crtc, conn,
						     mode,
						     &ctx);
	if (ret == -EDEADLK) {
		ret = drm_modeset_backoff(&ctx);
		if (!ret)
			goto retry_conn_enable;
	}
	KUNIT_ASSERT_EQ(test, ret, 0);

	state = drm_kunit_helper_atomic_state_alloc(test, drm, &ctx);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, state);

	conn_state = drm_atomic_get_connector_state(state, conn);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, conn_state);

	conn_state->hdmi.broadcast_rgb = DRM_HDMI_BROADCAST_RGB_FULL;

	ret = drm_atomic_check_only(state);
	KUNIT_ASSERT_EQ(test, ret, 0);

	conn_state = drm_atomic_get_new_connector_state(state, conn);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, conn_state);

	KUNIT_ASSERT_EQ(test,
			conn_state->hdmi.broadcast_rgb,
			DRM_HDMI_BROADCAST_RGB_FULL);

	KUNIT_EXPECT_FALSE(test, conn_state->hdmi.is_limited_range);

	drm_modeset_drop_locks(&ctx);
	drm_modeset_acquire_fini(&ctx);
}

/*
 * Test that for an HDMI connector, with an HDMI monitor, if the
 * Broadcast RGB property is set to limited with a mode that isn't the
 * VIC-1 mode, we will get a limited RGB Quantization Range.
 */
static void drm_test_check_broadcast_rgb_limited_cea_mode(struct kunit *test)
{
	struct drm_atomic_helper_connector_hdmi_priv *priv;
	struct drm_modeset_acquire_ctx ctx;
	struct drm_connector_state *conn_state;
	struct drm_atomic_commit *state;
	struct drm_display_mode *preferred;
	struct drm_connector *conn;
	struct drm_device *drm;
	struct drm_crtc *crtc;
	int ret;

	priv = drm_kunit_helper_connector_hdmi_init(test);
	KUNIT_ASSERT_NOT_NULL(test, priv);

	drm = &priv->drm;
	crtc = priv->crtc;
	conn = &priv->connector;
	KUNIT_ASSERT_TRUE(test, conn->display_info.is_hdmi);

	preferred = find_preferred_mode(conn);
	KUNIT_ASSERT_NOT_NULL(test, preferred);
	KUNIT_ASSERT_NE(test, drm_match_cea_mode(preferred), 1);

	drm_modeset_acquire_init(&ctx, 0);

retry_conn_enable:
	ret = drm_kunit_helper_enable_crtc_connector(test, drm,
						     crtc, conn,
						     preferred,
						     &ctx);
	if (ret == -EDEADLK) {
		ret = drm_modeset_backoff(&ctx);
		if (!ret)
			goto retry_conn_enable;
	}
	KUNIT_ASSERT_EQ(test, ret, 0);

	state = drm_kunit_helper_atomic_state_alloc(test, drm, &ctx);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, state);

	conn_state = drm_atomic_get_connector_state(state, conn);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, conn_state);

	conn_state->hdmi.broadcast_rgb = DRM_HDMI_BROADCAST_RGB_LIMITED;

	ret = drm_atomic_check_only(state);
	KUNIT_ASSERT_EQ(test, ret, 0);

	conn_state = drm_atomic_get_new_connector_state(state, conn);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, conn_state);

	KUNIT_ASSERT_EQ(test,
			conn_state->hdmi.broadcast_rgb,
			DRM_HDMI_BROADCAST_RGB_LIMITED);

	KUNIT_EXPECT_TRUE(test, conn_state->hdmi.is_limited_range);

	drm_modeset_drop_locks(&ctx);
	drm_modeset_acquire_fini(&ctx);
}

/*
 * Test that for an HDMI connector, with an HDMI monitor, if the
 * Broadcast RGB property is set to limited with a VIC-1 mode, we will
 * get a limited RGB Quantization Range.
 */
static void drm_test_check_broadcast_rgb_limited_cea_mode_vic_1(struct kunit *test)
{
	struct drm_atomic_helper_connector_hdmi_priv *priv;
	struct drm_modeset_acquire_ctx ctx;
	struct drm_connector_state *conn_state;
	struct drm_atomic_commit *state;
	struct drm_display_mode *mode;
	struct drm_connector *conn;
	struct drm_device *drm;
	struct drm_crtc *crtc;
	int ret;

	priv = drm_kunit_helper_connector_hdmi_init(test);
	KUNIT_ASSERT_NOT_NULL(test, priv);

	drm = &priv->drm;
	conn = &priv->connector;
	KUNIT_ASSERT_TRUE(test, conn->display_info.is_hdmi);

	drm_modeset_acquire_init(&ctx, 0);

	mode = drm_kunit_display_mode_from_cea_vic(test, drm, 1);
	KUNIT_ASSERT_NOT_NULL(test, mode);

	crtc = priv->crtc;

retry_conn_enable:
	ret = drm_kunit_helper_enable_crtc_connector(test, drm,
						     crtc, conn,
						     mode,
						     &ctx);
	if (ret == -EDEADLK) {
		ret = drm_modeset_backoff(&ctx);
		if (!ret)
			goto retry_conn_enable;
	}
	KUNIT_ASSERT_EQ(test, ret, 0);

	state = drm_kunit_helper_atomic_state_alloc(test, drm, &ctx);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, state);

	conn_state = drm_atomic_get_connector_state(state, conn);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, conn_state);

	conn_state->hdmi.broadcast_rgb = DRM_HDMI_BROADCAST_RGB_LIMITED;

	ret = drm_atomic_check_only(state);
	KUNIT_ASSERT_EQ(test, ret, 0);

	conn_state = drm_atomic_get_new_connector_state(state, conn);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, conn_state);

	KUNIT_ASSERT_EQ(test,
			conn_state->hdmi.broadcast_rgb,
			DRM_HDMI_BROADCAST_RGB_LIMITED);

	KUNIT_EXPECT_TRUE(test, conn_state->hdmi.is_limited_range);

	drm_modeset_drop_locks(&ctx);
	drm_modeset_acquire_fini(&ctx);
}

/*
 * Test that for an HDMI connector, with an HDMI monitor, we will
 * get a limited RGB Quantization Range with a YUV420 mode, no
 * matter what the value of the Broadcast RGB property is set to.
 */
static void drm_test_check_broadcast_rgb_cea_mode_yuv420(struct kunit *test)
{
	struct drm_connector_hdmi_funcs hdmi_funcs = dummy_connector_hdmi_funcs;
	struct drm_atomic_helper_connector_hdmi_priv *priv;
	enum drm_hdmi_broadcast_rgb broadcast_rgb;
	struct drm_modeset_acquire_ctx ctx;
	struct drm_connector_state *conn_state;
	struct drm_atomic_commit *state;
	struct drm_display_mode *mode;
	struct drm_connector *conn;
	struct drm_device *drm;
	struct drm_crtc *crtc;
	int ret;

	broadcast_rgb = *(enum drm_hdmi_broadcast_rgb *)test->param_value;
	hdmi_funcs.supported_formats = BIT(DRM_OUTPUT_COLOR_FORMAT_RGB444) |
				       BIT(DRM_OUTPUT_COLOR_FORMAT_YCBCR420);

	priv = drm_kunit_helper_connector_hdmi_init_with_edid_funcs(test,
				&hdmi_funcs,
				test_edid_hdmi_1080p_rgb_yuv_4k_yuv420_dc_max_200mhz);
	KUNIT_ASSERT_NOT_NULL(test, priv);

	drm = &priv->drm;
	crtc = priv->crtc;
	conn = &priv->connector;
	KUNIT_ASSERT_TRUE(test, conn->display_info.is_hdmi);

	mode = drm_kunit_display_mode_from_cea_vic(test, drm, 95);
	KUNIT_ASSERT_NOT_NULL(test, mode);

	drm_modeset_acquire_init(&ctx, 0);

retry_conn_enable:
	ret = drm_kunit_helper_enable_crtc_connector(test, drm, crtc, conn,
						     mode, &ctx);
	if (ret == -EDEADLK) {
		ret = drm_modeset_backoff(&ctx);
		if (!ret)
			goto retry_conn_enable;
	}
	KUNIT_ASSERT_EQ(test, ret, 0);

	state = drm_kunit_helper_atomic_state_alloc(test, drm, &ctx);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, state);

retry_conn_state:
	conn_state = drm_atomic_get_connector_state(state, conn);
	if (PTR_ERR(conn_state) == -EDEADLK) {
		drm_atomic_commit_clear(state);
		ret = drm_modeset_backoff(&ctx);
		if (!ret)
			goto retry_conn_state;
	}
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, conn_state);

	conn_state->hdmi.broadcast_rgb = broadcast_rgb;

	ret = drm_atomic_check_only(state);
	if (ret == -EDEADLK) {
		drm_atomic_commit_clear(state);
		ret = drm_modeset_backoff(&ctx);
		if (!ret)
			goto retry_conn_state;
	}
	KUNIT_ASSERT_EQ(test, ret, 0);

	conn_state = drm_atomic_get_new_connector_state(state, conn);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, conn_state);

	KUNIT_ASSERT_EQ(test, conn_state->hdmi.broadcast_rgb, broadcast_rgb);
	KUNIT_ASSERT_EQ(test, conn_state->hdmi.output_format, DRM_OUTPUT_COLOR_FORMAT_YCBCR420);

	KUNIT_EXPECT_TRUE(test, conn_state->hdmi.is_limited_range);

	drm_modeset_drop_locks(&ctx);
	drm_modeset_acquire_fini(&ctx);
}

static const enum drm_hdmi_broadcast_rgb check_broadcast_rgb_cea_mode_yuv420_tests[] = {
	DRM_HDMI_BROADCAST_RGB_AUTO,
	DRM_HDMI_BROADCAST_RGB_FULL,
	DRM_HDMI_BROADCAST_RGB_LIMITED,
};

static void
check_broadcast_rgb_cea_mode_yuv420_desc(const enum drm_hdmi_broadcast_rgb *broadcast_rgb,
					 char *desc)
{
	sprintf(desc, "%s", drm_hdmi_connector_get_broadcast_rgb_name(*broadcast_rgb));
}

KUNIT_ARRAY_PARAM(check_broadcast_rgb_cea_mode_yuv420,
		  check_broadcast_rgb_cea_mode_yuv420_tests,
		  check_broadcast_rgb_cea_mode_yuv420_desc);

/*
 * Test that if we change the maximum bpc property to a different value,
 * we trigger a mode change on the connector's CRTC, which will in turn
 * disable/enable the connector.
 */
static void drm_test_check_output_bpc_crtc_mode_changed(struct kunit *test)
{
	struct drm_connector_hdmi_funcs hdmi_funcs = dummy_connector_hdmi_funcs;
	struct drm_atomic_helper_connector_hdmi_priv *priv;
	struct drm_modeset_acquire_ctx ctx;
	struct drm_connector_state *old_conn_state;
	struct drm_connector_state *new_conn_state;
	struct drm_crtc_state *crtc_state;
	struct drm_atomic_commit *state;
	struct drm_display_mode *preferred;
	struct drm_connector *conn;
	struct drm_device *drm;
	struct drm_crtc *crtc;
	int ret;

	hdmi_funcs.max_bpc = 10;
	priv = drm_kunit_helper_connector_hdmi_init_with_edid_funcs(test,
				&hdmi_funcs,
				test_edid_hdmi_1080p_rgb_yuv_dc_max_200mhz);
	KUNIT_ASSERT_NOT_NULL(test, priv);

	drm = &priv->drm;
	crtc = priv->crtc;
	conn = &priv->connector;
	preferred = find_preferred_mode(conn);
	KUNIT_ASSERT_NOT_NULL(test, preferred);

	drm_modeset_acquire_init(&ctx, 0);

retry_conn_enable:
	ret = drm_kunit_helper_enable_crtc_connector(test, drm,
						     crtc, conn,
						     preferred,
						     &ctx);
	if (ret == -EDEADLK) {
		ret = drm_modeset_backoff(&ctx);
		if (!ret)
			goto retry_conn_enable;
	}
	KUNIT_ASSERT_EQ(test, ret, 0);

	state = drm_kunit_helper_atomic_state_alloc(test, drm, &ctx);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, state);

	new_conn_state = drm_atomic_get_connector_state(state, conn);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, new_conn_state);

	old_conn_state = drm_atomic_get_old_connector_state(state, conn);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, old_conn_state);

	new_conn_state->max_requested_bpc = 8;

	KUNIT_ASSERT_NE(test,
			old_conn_state->max_requested_bpc,
			new_conn_state->max_requested_bpc);

	ret = drm_atomic_check_only(state);
	KUNIT_ASSERT_EQ(test, ret, 0);

	old_conn_state = drm_atomic_get_old_connector_state(state, conn);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, old_conn_state);

	new_conn_state = drm_atomic_get_new_connector_state(state, conn);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, new_conn_state);

	KUNIT_ASSERT_NE(test,
			old_conn_state->hdmi.output_bpc,
			new_conn_state->hdmi.output_bpc);

	crtc_state = drm_atomic_get_new_crtc_state(state, crtc);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, crtc_state);
	KUNIT_EXPECT_TRUE(test, crtc_state->mode_changed);

	drm_modeset_drop_locks(&ctx);
	drm_modeset_acquire_fini(&ctx);
}

/*
 * Test that if we set the output bpc property to the same value, we
 * don't trigger a mode change on the connector's CRTC and leave the
 * connector unaffected.
 */
static void drm_test_check_output_bpc_crtc_mode_not_changed(struct kunit *test)
{
	struct drm_connector_hdmi_funcs hdmi_funcs = dummy_connector_hdmi_funcs;
	struct drm_atomic_helper_connector_hdmi_priv *priv;
	struct drm_modeset_acquire_ctx ctx;
	struct drm_connector_state *old_conn_state;
	struct drm_connector_state *new_conn_state;
	struct drm_crtc_state *crtc_state;
	struct drm_atomic_commit *state;
	struct drm_display_mode *preferred;
	struct drm_connector *conn;
	struct drm_device *drm;
	struct drm_crtc *crtc;
	int ret;

	hdmi_funcs.max_bpc = 10;
	priv = drm_kunit_helper_connector_hdmi_init_with_edid_funcs(test,
				&hdmi_funcs,
				test_edid_hdmi_1080p_rgb_yuv_dc_max_200mhz);
	KUNIT_ASSERT_NOT_NULL(test, priv);

	drm = &priv->drm;
	crtc = priv->crtc;
	conn = &priv->connector;
	preferred = find_preferred_mode(conn);
	KUNIT_ASSERT_NOT_NULL(test, preferred);

	drm_modeset_acquire_init(&ctx, 0);

retry_conn_enable:
	ret = drm_kunit_helper_enable_crtc_connector(test, drm,
						     crtc, conn,
						     preferred,
						     &ctx);
	if (ret == -EDEADLK) {
		ret = drm_modeset_backoff(&ctx);
		if (!ret)
			goto retry_conn_enable;
	}
	KUNIT_ASSERT_EQ(test, ret, 0);

	state = drm_kunit_helper_atomic_state_alloc(test, drm, &ctx);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, state);

	new_conn_state = drm_atomic_get_connector_state(state, conn);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, new_conn_state);

	old_conn_state = drm_atomic_get_old_connector_state(state, conn);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, old_conn_state);

	KUNIT_ASSERT_EQ(test,
			new_conn_state->hdmi.output_bpc,
			old_conn_state->hdmi.output_bpc);

	ret = drm_atomic_check_only(state);
	KUNIT_ASSERT_EQ(test, ret, 0);

	old_conn_state = drm_atomic_get_old_connector_state(state, conn);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, old_conn_state);

	new_conn_state = drm_atomic_get_new_connector_state(state, conn);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, new_conn_state);

	KUNIT_EXPECT_EQ(test,
			old_conn_state->hdmi.output_bpc,
			new_conn_state->hdmi.output_bpc);

	crtc_state = drm_atomic_get_new_crtc_state(state, crtc);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, crtc_state);
	KUNIT_EXPECT_FALSE(test, crtc_state->mode_changed);

	drm_modeset_drop_locks(&ctx);
	drm_modeset_acquire_fini(&ctx);
}

/*
 * Test that if we have an HDMI connector but a !HDMI display, we always
 * output RGB with 8 bpc.
 */
static void drm_test_check_output_bpc_dvi(struct kunit *test)
{
	struct drm_connector_hdmi_funcs hdmi_funcs = dummy_connector_hdmi_funcs;
	struct drm_atomic_helper_connector_hdmi_priv *priv;
	struct drm_modeset_acquire_ctx ctx;
	struct drm_connector_state *conn_state;
	struct drm_display_info *info;
	struct drm_display_mode *preferred;
	struct drm_connector *conn;
	struct drm_device *drm;
	struct drm_crtc *crtc;
	int ret;

	hdmi_funcs.max_bpc = 12;
	hdmi_funcs.supported_formats = BIT(DRM_OUTPUT_COLOR_FORMAT_RGB444) |
				       BIT(DRM_OUTPUT_COLOR_FORMAT_YCBCR422) |
				       BIT(DRM_OUTPUT_COLOR_FORMAT_YCBCR444);
	priv = drm_kunit_helper_connector_hdmi_init_with_edid_funcs(test,
				&hdmi_funcs,
				test_edid_dvi_1080p);
	KUNIT_ASSERT_NOT_NULL(test, priv);

	drm = &priv->drm;
	crtc = priv->crtc;
	conn = &priv->connector;
	info = &conn->display_info;
	KUNIT_ASSERT_FALSE(test, info->is_hdmi);

	preferred = find_preferred_mode(conn);
	KUNIT_ASSERT_NOT_NULL(test, preferred);

	drm_modeset_acquire_init(&ctx, 0);

retry_conn_enable:
	ret = drm_kunit_helper_enable_crtc_connector(test, drm,
						     crtc, conn,
						     preferred,
						     &ctx);
	if (ret == -EDEADLK) {
		ret = drm_modeset_backoff(&ctx);
		if (!ret)
			goto retry_conn_enable;
	}
	KUNIT_ASSERT_EQ(test, ret, 0);

	conn_state = conn->state;
	KUNIT_ASSERT_NOT_NULL(test, conn_state);

	KUNIT_EXPECT_EQ(test, conn_state->hdmi.output_bpc, 8);
	KUNIT_EXPECT_EQ(test, conn_state->hdmi.output_format, DRM_OUTPUT_COLOR_FORMAT_RGB444);

	drm_modeset_drop_locks(&ctx);
	drm_modeset_acquire_fini(&ctx);
}

/*
 * Test that when doing a commit which would use RGB 8bpc, the TMDS
 * clock rate stored in the connector state is equal to the mode clock
 */
static void drm_test_check_tmds_char_rate_rgb_8bpc(struct kunit *test)
{
	struct drm_atomic_helper_connector_hdmi_priv *priv;
	struct drm_modeset_acquire_ctx ctx;
	struct drm_connector_state *conn_state;
	struct drm_display_mode *preferred;
	struct drm_connector *conn;
	struct drm_device *drm;
	struct drm_crtc *crtc;
	int ret;

	priv = drm_kunit_helper_connector_hdmi_init_with_edid_funcs(test,
				&dummy_connector_hdmi_funcs,
				test_edid_hdmi_1080p_rgb_max_200mhz);
	KUNIT_ASSERT_NOT_NULL(test, priv);

	drm = &priv->drm;
	crtc = priv->crtc;
	conn = &priv->connector;
	preferred = find_preferred_mode(conn);
	KUNIT_ASSERT_NOT_NULL(test, preferred);
	KUNIT_ASSERT_FALSE(test, preferred->flags & DRM_MODE_FLAG_DBLCLK);

	drm_modeset_acquire_init(&ctx, 0);

retry_conn_enable:
	ret = drm_kunit_helper_enable_crtc_connector(test, drm,
						     crtc, conn,
						     preferred,
						     &ctx);
	if (ret == -EDEADLK) {
		ret = drm_modeset_backoff(&ctx);
		if (!ret)
			goto retry_conn_enable;
	}
	KUNIT_ASSERT_EQ(test, ret, 0);

	conn_state = conn->state;
	KUNIT_ASSERT_NOT_NULL(test, conn_state);

	KUNIT_ASSERT_EQ(test, conn_state->hdmi.output_bpc, 8);
	KUNIT_ASSERT_EQ(test, conn_state->hdmi.output_format, DRM_OUTPUT_COLOR_FORMAT_RGB444);
	KUNIT_EXPECT_EQ(test, conn_state->hdmi.tmds_char_rate, preferred->clock * 1000);

	drm_modeset_drop_locks(&ctx);
	drm_modeset_acquire_fini(&ctx);
}

/*
 * Test that when doing a commit which would use RGB 10bpc, the TMDS
 * clock rate stored in the connector state is equal to 1.25 times the
 * mode pixel clock
 */
static void drm_test_check_tmds_char_rate_rgb_10bpc(struct kunit *test)
{
	struct drm_connector_hdmi_funcs hdmi_funcs = dummy_connector_hdmi_funcs;
	struct drm_atomic_helper_connector_hdmi_priv *priv;
	struct drm_modeset_acquire_ctx ctx;
	struct drm_connector_state *conn_state;
	struct drm_display_mode *preferred;
	struct drm_connector *conn;
	struct drm_device *drm;
	struct drm_crtc *crtc;
	int ret;

	hdmi_funcs.max_bpc = 10;
	priv = drm_kunit_helper_connector_hdmi_init_with_edid_funcs(test,
				&hdmi_funcs,
				test_edid_hdmi_1080p_rgb_yuv_dc_max_340mhz);
	KUNIT_ASSERT_NOT_NULL(test, priv);

	drm = &priv->drm;
	crtc = priv->crtc;
	conn = &priv->connector;
	preferred = find_preferred_mode(conn);
	KUNIT_ASSERT_NOT_NULL(test, preferred);
	KUNIT_ASSERT_FALSE(test, preferred->flags & DRM_MODE_FLAG_DBLCLK);

	drm_modeset_acquire_init(&ctx, 0);

retry_conn_enable:
	ret = drm_kunit_helper_enable_crtc_connector(test, drm,
						     crtc, conn,
						     preferred,
						     &ctx);
	if (ret == -EDEADLK) {
		ret = drm_modeset_backoff(&ctx);
		if (!ret)
			goto retry_conn_enable;
	}
	KUNIT_ASSERT_EQ(test, ret, 0);

	conn_state = conn->state;
	KUNIT_ASSERT_NOT_NULL(test, conn_state);

	KUNIT_ASSERT_EQ(test, conn_state->hdmi.output_bpc, 10);
	KUNIT_ASSERT_EQ(test, conn_state->hdmi.output_format, DRM_OUTPUT_COLOR_FORMAT_RGB444);
	KUNIT_EXPECT_EQ(test, conn_state->hdmi.tmds_char_rate, preferred->clock * 1250);

	drm_modeset_drop_locks(&ctx);
	drm_modeset_acquire_fini(&ctx);
}

/*
 * Test that when doing a commit which would use RGB 12bpc, the TMDS
 * clock rate stored in the connector state is equal to 1.5 times the
 * mode pixel clock
 */
static void drm_test_check_tmds_char_rate_rgb_12bpc(struct kunit *test)
{
	struct drm_connector_hdmi_funcs hdmi_funcs = dummy_connector_hdmi_funcs;
	struct drm_atomic_helper_connector_hdmi_priv *priv;
	struct drm_modeset_acquire_ctx ctx;
	struct drm_connector_state *conn_state;
	struct drm_display_mode *preferred;
	struct drm_connector *conn;
	struct drm_device *drm;
	struct drm_crtc *crtc;
	int ret;

	hdmi_funcs.max_bpc = 12;
	priv = drm_kunit_helper_connector_hdmi_init_with_edid_funcs(test,
				&hdmi_funcs,
				test_edid_hdmi_1080p_rgb_yuv_dc_max_340mhz);
	KUNIT_ASSERT_NOT_NULL(test, priv);

	drm = &priv->drm;
	crtc = priv->crtc;
	conn = &priv->connector;
	preferred = find_preferred_mode(conn);
	KUNIT_ASSERT_NOT_NULL(test, preferred);
	KUNIT_ASSERT_FALSE(test, preferred->flags & DRM_MODE_FLAG_DBLCLK);

	drm_modeset_acquire_init(&ctx, 0);

retry_conn_enable:
	ret = drm_kunit_helper_enable_crtc_connector(test, drm,
						     crtc, conn,
						     preferred,
						     &ctx);
	if (ret == -EDEADLK) {
		ret = drm_modeset_backoff(&ctx);
		if (!ret)
			goto retry_conn_enable;
	}
	KUNIT_ASSERT_EQ(test, ret, 0);

	conn_state = conn->state;
	KUNIT_ASSERT_NOT_NULL(test, conn_state);

	KUNIT_ASSERT_EQ(test, conn_state->hdmi.output_bpc, 12);
	KUNIT_ASSERT_EQ(test, conn_state->hdmi.output_format, DRM_OUTPUT_COLOR_FORMAT_RGB444);
	KUNIT_EXPECT_EQ(test, conn_state->hdmi.tmds_char_rate, preferred->clock * 1500);

	drm_modeset_drop_locks(&ctx);
	drm_modeset_acquire_fini(&ctx);
}

/*
 * Test that if we filter a rate through our hook, it's indeed rejected
 * by the whole atomic_check logic.
 *
 * We do so by first doing a commit on the pipeline to make sure that it
 * works, change the HDMI helpers pointer, and then try the same commit
 * again to see if it fails as it should.
 */
static void drm_test_check_hdmi_funcs_reject_rate(struct kunit *test)
{
	struct drm_atomic_helper_connector_hdmi_priv *priv;
	struct drm_modeset_acquire_ctx ctx;
	struct drm_atomic_commit *state;
	struct drm_display_mode *preferred;
	struct drm_crtc_state *crtc_state;
	struct drm_connector *conn;
	struct drm_device *drm;
	struct drm_crtc *crtc;
	int ret;

	priv = drm_kunit_helper_connector_hdmi_init(test);
	KUNIT_ASSERT_NOT_NULL(test, priv);

	drm = &priv->drm;
	crtc = priv->crtc;
	conn = &priv->connector;

	preferred = find_preferred_mode(conn);
	KUNIT_ASSERT_NOT_NULL(test, preferred);

	drm_modeset_acquire_init(&ctx, 0);

retry_conn_enable:
	ret = drm_kunit_helper_enable_crtc_connector(test, drm,
						     crtc, conn,
						     preferred,
						     &ctx);
	if (ret == -EDEADLK) {
		ret = drm_modeset_backoff(&ctx);
		if (!ret)
			goto retry_conn_enable;
	}
	KUNIT_ASSERT_EQ(test, ret, 0);

	/* You shouldn't be doing that at home. */
	conn->hdmi.funcs = &reject_connector_hdmi_funcs;

	state = drm_kunit_helper_atomic_state_alloc(test, drm, &ctx);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, state);

	crtc_state = drm_atomic_get_crtc_state(state, crtc);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, crtc_state);

	crtc_state->connectors_changed = true;

	ret = drm_atomic_check_only(state);
	KUNIT_EXPECT_LT(test, ret, 0);

	drm_modeset_drop_locks(&ctx);
	drm_modeset_acquire_fini(&ctx);
}

/*
 * Test that if:
 * - We have an HDMI connector supporting RGB only
 * - The chosen mode has a TMDS character rate higher than the display
 *   supports in RGB/12bpc
 * - The chosen mode has a TMDS character rate lower than the display
 *   supports in RGB/10bpc.
 *
 * Then we will pick the latter, and the computed TMDS character rate
 * will be equal to 1.25 times the mode pixel clock.
 */
static void drm_test_check_max_tmds_rate_bpc_fallback_rgb(struct kunit *test)
{
	struct drm_connector_hdmi_funcs hdmi_funcs = dummy_connector_hdmi_funcs;
	struct drm_atomic_helper_connector_hdmi_priv *priv;
	struct drm_modeset_acquire_ctx ctx;
	struct drm_connector_state *conn_state;
	struct drm_display_info *info;
	struct drm_display_mode *preferred;
	unsigned long long rate;
	struct drm_connector *conn;
	struct drm_device *drm;
	struct drm_crtc *crtc;
	int ret;

	hdmi_funcs.max_bpc = 12;
	priv = drm_kunit_helper_connector_hdmi_init_with_edid_funcs(test,
				&hdmi_funcs,
				test_edid_hdmi_1080p_rgb_yuv_dc_max_200mhz);
	KUNIT_ASSERT_NOT_NULL(test, priv);

	drm = &priv->drm;
	crtc = priv->crtc;
	conn = &priv->connector;
	info = &conn->display_info;
	KUNIT_ASSERT_TRUE(test, info->is_hdmi);
	KUNIT_ASSERT_GT(test, info->max_tmds_clock, 0);

	preferred = find_preferred_mode(conn);
	KUNIT_ASSERT_NOT_NULL(test, preferred);
	KUNIT_ASSERT_FALSE(test, preferred->flags & DRM_MODE_FLAG_DBLCLK);

	rate = drm_hdmi_compute_mode_clock(preferred, 12, DRM_OUTPUT_COLOR_FORMAT_RGB444);
	KUNIT_ASSERT_GT(test, rate, info->max_tmds_clock * 1000);

	rate = drm_hdmi_compute_mode_clock(preferred, 10, DRM_OUTPUT_COLOR_FORMAT_RGB444);
	KUNIT_ASSERT_LT(test, rate, info->max_tmds_clock * 1000);

	drm_modeset_acquire_init(&ctx, 0);

retry_conn_enable:
	ret = drm_kunit_helper_enable_crtc_connector(test, drm,
						     crtc, conn,
						     preferred,
						     &ctx);
	if (ret == -EDEADLK) {
		ret = drm_modeset_backoff(&ctx);
		if (!ret)
			goto retry_conn_enable;
	}
	KUNIT_EXPECT_EQ(test, ret, 0);

	conn_state = conn->state;
	KUNIT_ASSERT_NOT_NULL(test, conn_state);

	KUNIT_EXPECT_EQ(test, conn_state->hdmi.output_bpc, 10);
	KUNIT_EXPECT_EQ(test, conn_state->hdmi.output_format, DRM_OUTPUT_COLOR_FORMAT_RGB444);
	KUNIT_EXPECT_EQ(test, conn_state->hdmi.tmds_char_rate, preferred->clock * 1250);

	drm_modeset_drop_locks(&ctx);
	drm_modeset_acquire_fini(&ctx);
}

/*
 * Test that if:
 * - We have an HDMI connector and a display supporting both RGB and YUV420
 * - The chosen mode can be supported in YUV420 output format only
 * - The chosen mode has a TMDS character rate higher than the display
 *   supports in YUV420/12bpc
 * - The chosen mode has a TMDS character rate lower than the display
 *   supports in YUV420/10bpc.
 *
 * Then we will pick the latter, and the computed TMDS character rate
 * will be equal to 1.25 * 0.5 times the mode pixel clock.
 */
static void drm_test_check_max_tmds_rate_bpc_fallback_yuv420(struct kunit *test)
{
	struct drm_connector_hdmi_funcs hdmi_funcs = dummy_connector_hdmi_funcs;
	struct drm_atomic_helper_connector_hdmi_priv *priv;
	struct drm_modeset_acquire_ctx ctx;
	struct drm_connector_state *conn_state;
	struct drm_display_info *info;
	struct drm_display_mode *yuv420_only_mode;
	unsigned long long rate;
	struct drm_connector *conn;
	struct drm_device *drm;
	struct drm_crtc *crtc;
	int ret;

	hdmi_funcs.max_bpc = 12;
	hdmi_funcs.supported_formats = BIT(DRM_OUTPUT_COLOR_FORMAT_RGB444) |
				       BIT(DRM_OUTPUT_COLOR_FORMAT_YCBCR420);
	priv = drm_kunit_helper_connector_hdmi_init_with_edid_funcs(test,
				&hdmi_funcs,
				test_edid_hdmi_1080p_rgb_yuv_4k_yuv420_dc_max_200mhz);
	KUNIT_ASSERT_NOT_NULL(test, priv);

	drm = &priv->drm;
	crtc = priv->crtc;
	conn = &priv->connector;
	info = &conn->display_info;
	KUNIT_ASSERT_TRUE(test, info->is_hdmi);
	KUNIT_ASSERT_GT(test, info->max_tmds_clock, 0);
	KUNIT_ASSERT_TRUE(test, conn->ycbcr_420_allowed);

	yuv420_only_mode = drm_kunit_display_mode_from_cea_vic(test, drm, 95);
	KUNIT_ASSERT_NOT_NULL(test, yuv420_only_mode);
	KUNIT_ASSERT_TRUE(test, drm_mode_is_420_only(info, yuv420_only_mode));

	rate = drm_hdmi_compute_mode_clock(yuv420_only_mode, 12, DRM_OUTPUT_COLOR_FORMAT_YCBCR420);
	KUNIT_ASSERT_GT(test, rate, info->max_tmds_clock * 1000);

	rate = drm_hdmi_compute_mode_clock(yuv420_only_mode, 10, DRM_OUTPUT_COLOR_FORMAT_YCBCR420);
	KUNIT_ASSERT_LT(test, rate, info->max_tmds_clock * 1000);

	drm_modeset_acquire_init(&ctx, 0);

retry_conn_enable:
	ret = drm_kunit_helper_enable_crtc_connector(test, drm, crtc, conn,
						     yuv420_only_mode, &ctx);
	if (ret == -EDEADLK) {
		ret = drm_modeset_backoff(&ctx);
		if (!ret)
			goto retry_conn_enable;
	}
	KUNIT_EXPECT_EQ(test, ret, 0);

	conn_state = conn->state;
	KUNIT_ASSERT_NOT_NULL(test, conn_state);

	KUNIT_EXPECT_EQ(test, conn_state->hdmi.output_bpc, 10);
	KUNIT_EXPECT_EQ(test, conn_state->hdmi.output_format, DRM_OUTPUT_COLOR_FORMAT_YCBCR420);
	KUNIT_EXPECT_EQ(test, conn_state->hdmi.tmds_char_rate, yuv420_only_mode->clock * 625);

	drm_modeset_drop_locks(&ctx);
	drm_modeset_acquire_fini(&ctx);
}

/*
 * Test that if:
 * - We have an HDMI connector supporting both RGB and YUV422 and up to
 *   12 bpc
 * - The chosen mode has a TMDS character rate higher than the display
 *   supports in RGB/12bpc but lower than the display supports in
 *   RGB/10bpc
 * - The chosen mode has a TMDS character rate lower than the display
 *   supports in YUV422/12bpc.
 * - The HDMI connector state's color format property is unset (i.e. AUTO)
 *
 * Then we will prefer to keep the RGB format with a lower bpc over
 * picking YUV422.
 */
static void drm_test_check_max_tmds_rate_bpc_fallback_ignore_yuv422(struct kunit *test)
{
	struct drm_connector_hdmi_funcs hdmi_funcs = dummy_connector_hdmi_funcs;
	struct drm_atomic_helper_connector_hdmi_priv *priv;
	struct drm_modeset_acquire_ctx ctx;
	struct drm_connector_state *conn_state;
	struct drm_display_info *info;
	struct drm_display_mode *preferred;
	unsigned long long rate;
	struct drm_connector *conn;
	struct drm_device *drm;
	struct drm_crtc *crtc;
	int ret;

	hdmi_funcs.max_bpc = 12;
	hdmi_funcs.supported_formats = BIT(DRM_OUTPUT_COLOR_FORMAT_RGB444) |
				       BIT(DRM_OUTPUT_COLOR_FORMAT_YCBCR422) |
				       BIT(DRM_OUTPUT_COLOR_FORMAT_YCBCR444);
	priv = drm_kunit_helper_connector_hdmi_init_with_edid_funcs(test,
				&hdmi_funcs,
				test_edid_hdmi_1080p_rgb_yuv_dc_max_200mhz);
	KUNIT_ASSERT_NOT_NULL(test, priv);

	drm = &priv->drm;
	crtc = priv->crtc;
	conn = &priv->connector;
	info = &conn->display_info;
	KUNIT_ASSERT_TRUE(test, info->is_hdmi);
	KUNIT_ASSERT_GT(test, info->max_tmds_clock, 0);

	preferred = find_preferred_mode(conn);
	KUNIT_ASSERT_NOT_NULL(test, preferred);
	KUNIT_ASSERT_FALSE(test, preferred->flags & DRM_MODE_FLAG_DBLCLK);

	rate = drm_hdmi_compute_mode_clock(preferred, 10, DRM_OUTPUT_COLOR_FORMAT_RGB444);
	KUNIT_ASSERT_LT(test, rate, info->max_tmds_clock * 1000);

	rate = drm_hdmi_compute_mode_clock(preferred, 12, DRM_OUTPUT_COLOR_FORMAT_RGB444);
	KUNIT_ASSERT_GT(test, rate, info->max_tmds_clock * 1000);

	rate = drm_hdmi_compute_mode_clock(preferred, 12, DRM_OUTPUT_COLOR_FORMAT_YCBCR422);
	KUNIT_ASSERT_LT(test, rate, info->max_tmds_clock * 1000);

	drm_modeset_acquire_init(&ctx, 0);

retry_conn_enable:
	ret = drm_kunit_helper_enable_crtc_connector(test, drm,
						     crtc, conn,
						     preferred,
						     &ctx);
	if (ret == -EDEADLK) {
		ret = drm_modeset_backoff(&ctx);
		if (!ret)
			goto retry_conn_enable;
	}
	KUNIT_EXPECT_EQ(test, ret, 0);

	conn_state = conn->state;
	KUNIT_ASSERT_NOT_NULL(test, conn_state);
	KUNIT_ASSERT_EQ(test, conn_state->color_format, DRM_CONNECTOR_COLOR_FORMAT_AUTO);

	KUNIT_EXPECT_EQ(test, conn_state->hdmi.output_bpc, 10);
	KUNIT_EXPECT_EQ(test, conn_state->hdmi.output_format, DRM_OUTPUT_COLOR_FORMAT_RGB444);

	drm_modeset_drop_locks(&ctx);
	drm_modeset_acquire_fini(&ctx);
}

/*
 * Test that if:
 * - We have an HDMI connector supporting both RGB and YUV420 and up to
 *   12 bpc
 * - The chosen mode has a TMDS character rate higher than the display
 *   supports in RGB/10bpc but lower than the display supports in
 *   RGB/8bpc
 * - The chosen mode has a TMDS character rate lower than the display
 *   supports in YUV420/12bpc.
 * - The HDMI connector state's color format property is unset (i.e. AUTO)
 *
 * Then we will prefer to keep the RGB format with a lower bpc over
 * picking YUV420.
 */
static void drm_test_check_max_tmds_rate_bpc_fallback_ignore_yuv420(struct kunit *test)
{
	struct drm_connector_hdmi_funcs hdmi_funcs = dummy_connector_hdmi_funcs;
	struct drm_atomic_helper_connector_hdmi_priv *priv;
	struct drm_modeset_acquire_ctx ctx;
	struct drm_connector_state *conn_state;
	struct drm_display_info *info;
	struct drm_display_mode *preferred;
	unsigned long long rate;
	struct drm_connector *conn;
	struct drm_device *drm;
	struct drm_crtc *crtc;
	int ret;

	hdmi_funcs.max_bpc = 12;
	hdmi_funcs.supported_formats = BIT(DRM_OUTPUT_COLOR_FORMAT_RGB444) |
				       BIT(DRM_OUTPUT_COLOR_FORMAT_YCBCR420);
	priv = drm_kunit_helper_connector_hdmi_init_with_edid_funcs(test,
				&hdmi_funcs,
				test_edid_hdmi_4k_rgb_yuv420_dc_max_340mhz);
	KUNIT_ASSERT_NOT_NULL(test, priv);

	drm = &priv->drm;
	crtc = priv->crtc;
	conn = &priv->connector;
	info = &conn->display_info;
	KUNIT_ASSERT_TRUE(test, info->is_hdmi);
	KUNIT_ASSERT_GT(test, info->max_tmds_clock, 0);
	KUNIT_ASSERT_TRUE(test, conn->ycbcr_420_allowed);

	preferred = find_preferred_mode(conn);
	KUNIT_ASSERT_NOT_NULL(test, preferred);
	KUNIT_ASSERT_FALSE(test, preferred->flags & DRM_MODE_FLAG_DBLCLK);
	KUNIT_ASSERT_TRUE(test, drm_mode_is_420_also(info, preferred));

	rate = drm_hdmi_compute_mode_clock(preferred, 8, DRM_OUTPUT_COLOR_FORMAT_RGB444);
	KUNIT_ASSERT_LT(test, rate, info->max_tmds_clock * 1000);

	rate = drm_hdmi_compute_mode_clock(preferred, 10, DRM_OUTPUT_COLOR_FORMAT_RGB444);
	KUNIT_ASSERT_GT(test, rate, info->max_tmds_clock * 1000);

	rate = drm_hdmi_compute_mode_clock(preferred, 12, DRM_OUTPUT_COLOR_FORMAT_YCBCR420);
	KUNIT_ASSERT_LT(test, rate, info->max_tmds_clock * 1000);

	drm_modeset_acquire_init(&ctx, 0);

retry_conn_enable:
	ret = drm_kunit_helper_enable_crtc_connector(test, drm, crtc, conn,
						     preferred, &ctx);
	if (ret == -EDEADLK) {
		ret = drm_modeset_backoff(&ctx);
		if (!ret)
			goto retry_conn_enable;
	}
	KUNIT_EXPECT_EQ(test, ret, 0);

	conn_state = conn->state;
	KUNIT_ASSERT_NOT_NULL(test, conn_state);
	KUNIT_ASSERT_EQ(test, conn_state->color_format, DRM_CONNECTOR_COLOR_FORMAT_AUTO);

	KUNIT_EXPECT_EQ(test, conn_state->hdmi.output_bpc, 8);
	KUNIT_EXPECT_EQ(test, conn_state->hdmi.output_format, DRM_OUTPUT_COLOR_FORMAT_RGB444);

	drm_modeset_drop_locks(&ctx);
	drm_modeset_acquire_fini(&ctx);
}

/*
 * Test that if a driver supports only RGB, but the chosen mode can be
 * supported by the screen only in YUV420 output format, we end up with
 * unsuccessful fallback attempts.
 */
static void drm_test_check_driver_unsupported_fallback_yuv420(struct kunit *test)
{
	struct drm_connector_hdmi_funcs hdmi_funcs = dummy_connector_hdmi_funcs;
	struct drm_atomic_helper_connector_hdmi_priv *priv;
	struct drm_modeset_acquire_ctx ctx;
	struct drm_connector_state *conn_state;
	struct drm_crtc_state *crtc_state;
	struct drm_atomic_commit *state;
	struct drm_display_info *info;
	struct drm_display_mode *preferred, *yuv420_only_mode;
	struct drm_connector *conn;
	struct drm_device *drm;
	struct drm_crtc *crtc;
	int ret;

	hdmi_funcs.max_bpc = 12;
	priv = drm_kunit_helper_connector_hdmi_init_with_edid_funcs(test,
				&hdmi_funcs,
				test_edid_hdmi_1080p_rgb_yuv_4k_yuv420_dc_max_200mhz);
	KUNIT_ASSERT_NOT_NULL(test, priv);

	drm = &priv->drm;
	crtc = priv->crtc;
	conn = &priv->connector;
	info = &conn->display_info;
	KUNIT_ASSERT_TRUE(test, info->is_hdmi);
	KUNIT_ASSERT_FALSE(test, conn->ycbcr_420_allowed);

	preferred = find_preferred_mode(conn);
	KUNIT_ASSERT_NOT_NULL(test, preferred);
	KUNIT_ASSERT_FALSE(test, drm_mode_is_420_also(info, preferred));

	yuv420_only_mode = drm_kunit_display_mode_from_cea_vic(test, drm, 95);
	KUNIT_ASSERT_NOT_NULL(test, yuv420_only_mode);
	KUNIT_ASSERT_TRUE(test, drm_mode_is_420_only(info, yuv420_only_mode));

	drm_modeset_acquire_init(&ctx, 0);

retry_conn_enable:
	ret = drm_kunit_helper_enable_crtc_connector(test, drm, crtc, conn,
						     preferred, &ctx);
	if (ret == -EDEADLK) {
		ret = drm_modeset_backoff(&ctx);
		if (!ret)
			goto retry_conn_enable;
	}
	KUNIT_EXPECT_EQ(test, ret, 0);

	conn_state = conn->state;
	KUNIT_ASSERT_NOT_NULL(test, conn_state);
	KUNIT_EXPECT_EQ(test, conn_state->hdmi.output_format, DRM_OUTPUT_COLOR_FORMAT_RGB444);

	state = drm_kunit_helper_atomic_state_alloc(test, drm, &ctx);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, state);

retry_crtc_state:
	crtc_state = drm_atomic_get_crtc_state(state, crtc);
	if (PTR_ERR(crtc_state) == -EDEADLK) {
		drm_atomic_commit_clear(state);
		ret = drm_modeset_backoff(&ctx);
		if (!ret)
			goto retry_crtc_state;
	}
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, crtc_state);

	ret = drm_atomic_set_mode_for_crtc(crtc_state, yuv420_only_mode);
	KUNIT_EXPECT_EQ(test, ret, 0);

	ret = drm_atomic_check_only(state);
	if (ret == -EDEADLK) {
		drm_atomic_commit_clear(state);
		ret = drm_modeset_backoff(&ctx);
		if (!ret)
			goto retry_crtc_state;
	}
	KUNIT_ASSERT_LT(test, ret, 0);

	drm_modeset_drop_locks(&ctx);
	drm_modeset_acquire_fini(&ctx);
}

/*
 * Test that if a driver and screen supports RGB and YUV formats, and we
 * try to set the VIC 1 mode, we end up with 8bpc RGB even if we could
 * have had a higher bpc.
 */
static void drm_test_check_output_bpc_format_vic_1(struct kunit *test)
{
	struct drm_connector_hdmi_funcs hdmi_funcs = dummy_connector_hdmi_funcs;
	struct drm_atomic_helper_connector_hdmi_priv *priv;
	struct drm_modeset_acquire_ctx ctx;
	struct drm_connector_state *conn_state;
	struct drm_display_info *info;
	struct drm_display_mode *mode;
	unsigned long long rate;
	struct drm_connector *conn;
	struct drm_device *drm;
	struct drm_crtc *crtc;
	int ret;

	hdmi_funcs.max_bpc = 12;
	hdmi_funcs.supported_formats = BIT(DRM_OUTPUT_COLOR_FORMAT_RGB444) |
				       BIT(DRM_OUTPUT_COLOR_FORMAT_YCBCR422) |
				       BIT(DRM_OUTPUT_COLOR_FORMAT_YCBCR444);
	priv = drm_kunit_helper_connector_hdmi_init_with_edid_funcs(test,
				&hdmi_funcs,
				test_edid_hdmi_1080p_rgb_yuv_dc_max_200mhz);
	KUNIT_ASSERT_NOT_NULL(test, priv);

	drm = &priv->drm;
	conn = &priv->connector;
	info = &conn->display_info;
	KUNIT_ASSERT_TRUE(test, info->is_hdmi);
	KUNIT_ASSERT_GT(test, info->max_tmds_clock, 0);

	mode = drm_kunit_display_mode_from_cea_vic(test, drm, 1);
	KUNIT_ASSERT_NOT_NULL(test, mode);

	/*
	 * NOTE: We can't use drm_hdmi_compute_mode_clock()
	 * here because we're trying to get the rate of an invalid
	 * configuration.
	 *
	 * Thus, we have to calculate the rate by hand.
	 */
	rate = mode->clock * 1500;
	KUNIT_ASSERT_LT(test, rate, info->max_tmds_clock * 1000);

	drm_modeset_acquire_init(&ctx, 0);

	crtc = priv->crtc;

retry_conn_enable:
	ret = drm_kunit_helper_enable_crtc_connector(test, drm,
						     crtc, conn,
						     mode,
						     &ctx);
	if (ret == -EDEADLK) {
		ret = drm_modeset_backoff(&ctx);
		if (!ret)
			goto retry_conn_enable;
	}
	KUNIT_EXPECT_EQ(test, ret, 0);

	conn_state = conn->state;
	KUNIT_ASSERT_NOT_NULL(test, conn_state);

	KUNIT_EXPECT_EQ(test, conn_state->hdmi.output_bpc, 8);
	KUNIT_EXPECT_EQ(test, conn_state->hdmi.output_format, DRM_OUTPUT_COLOR_FORMAT_RGB444);

	drm_modeset_drop_locks(&ctx);
	drm_modeset_acquire_fini(&ctx);
}

/*
 * Test that if a driver supports only RGB but the screen also supports
 * YUV formats, we only end up with an RGB format.
 */
static void drm_test_check_output_bpc_format_driver_rgb_only(struct kunit *test)
{
	struct drm_connector_hdmi_funcs hdmi_funcs = dummy_connector_hdmi_funcs;
	struct drm_atomic_helper_connector_hdmi_priv *priv;
	struct drm_modeset_acquire_ctx ctx;
	struct drm_connector_state *conn_state;
	struct drm_display_info *info;
	struct drm_display_mode *preferred;
	unsigned long long rate;
	struct drm_connector *conn;
	struct drm_device *drm;
	struct drm_crtc *crtc;
	int ret;

	hdmi_funcs.max_bpc = 12;
	priv = drm_kunit_helper_connector_hdmi_init_with_edid_funcs(test,
				&hdmi_funcs,
				test_edid_hdmi_1080p_rgb_yuv_dc_max_200mhz);
	KUNIT_ASSERT_NOT_NULL(test, priv);

	drm = &priv->drm;
	crtc = priv->crtc;
	conn = &priv->connector;
	info = &conn->display_info;
	KUNIT_ASSERT_TRUE(test, info->is_hdmi);
	KUNIT_ASSERT_GT(test, info->max_tmds_clock, 0);

	preferred = find_preferred_mode(conn);
	KUNIT_ASSERT_NOT_NULL(test, preferred);

	/*
	 * We're making sure that YUV422 would be the preferred option
	 * here: we're always favouring higher bpc, we can't have RGB
	 * because the TMDS character rate exceeds the maximum supported
	 * by the display, and YUV422 works for that display.
	 *
	 * But since the driver only supports RGB, we should fallback to
	 * a lower bpc with RGB.
	 */
	rate = drm_hdmi_compute_mode_clock(preferred, 12, DRM_OUTPUT_COLOR_FORMAT_RGB444);
	KUNIT_ASSERT_GT(test, rate, info->max_tmds_clock * 1000);

	rate = drm_hdmi_compute_mode_clock(preferred, 12, DRM_OUTPUT_COLOR_FORMAT_YCBCR422);
	KUNIT_ASSERT_LT(test, rate, info->max_tmds_clock * 1000);

	drm_modeset_acquire_init(&ctx, 0);

retry_conn_enable:
	ret = drm_kunit_helper_enable_crtc_connector(test, drm,
						     crtc, conn,
						     preferred,
						     &ctx);
	if (ret == -EDEADLK) {
		ret = drm_modeset_backoff(&ctx);
		if (!ret)
			goto retry_conn_enable;
	}
	KUNIT_EXPECT_EQ(test, ret, 0);

	conn_state = conn->state;
	KUNIT_ASSERT_NOT_NULL(test, conn_state);

	KUNIT_EXPECT_LT(test, conn_state->hdmi.output_bpc, 12);
	KUNIT_EXPECT_EQ(test, conn_state->hdmi.output_format, DRM_OUTPUT_COLOR_FORMAT_RGB444);

	drm_modeset_drop_locks(&ctx);
	drm_modeset_acquire_fini(&ctx);
}

/*
 * Test that if a screen supports only RGB but the driver also supports
 * YUV formats, we only end up with an RGB format.
 */
static void drm_test_check_output_bpc_format_display_rgb_only(struct kunit *test)
{
	struct drm_connector_hdmi_funcs hdmi_funcs = dummy_connector_hdmi_funcs;
	struct drm_atomic_helper_connector_hdmi_priv *priv;
	struct drm_modeset_acquire_ctx ctx;
	struct drm_connector_state *conn_state;
	struct drm_display_info *info;
	struct drm_display_mode *preferred;
	unsigned long long rate;
	struct drm_connector *conn;
	struct drm_device *drm;
	struct drm_crtc *crtc;
	int ret;

	hdmi_funcs.max_bpc = 12;
	hdmi_funcs.supported_formats = BIT(DRM_OUTPUT_COLOR_FORMAT_RGB444) |
				       BIT(DRM_OUTPUT_COLOR_FORMAT_YCBCR422) |
				       BIT(DRM_OUTPUT_COLOR_FORMAT_YCBCR444);
	priv = drm_kunit_helper_connector_hdmi_init_with_edid_funcs(test,
				&hdmi_funcs,
				test_edid_hdmi_1080p_rgb_max_200mhz);
	KUNIT_ASSERT_NOT_NULL(test, priv);

	drm = &priv->drm;
	crtc = priv->crtc;
	conn = &priv->connector;
	info = &conn->display_info;
	KUNIT_ASSERT_TRUE(test, info->is_hdmi);
	KUNIT_ASSERT_GT(test, info->max_tmds_clock, 0);

	preferred = find_preferred_mode(conn);
	KUNIT_ASSERT_NOT_NULL(test, preferred);

	/*
	 * We're making sure that YUV422 would be the preferred option
	 * here: we're always favouring higher bpc, we can't have RGB
	 * because the TMDS character rate exceeds the maximum supported
	 * by the display, and YUV422 works for that display.
	 *
	 * But since the display only supports RGB, we should fallback to
	 * a lower bpc with RGB.
	 */
	rate = drm_hdmi_compute_mode_clock(preferred, 12, DRM_OUTPUT_COLOR_FORMAT_RGB444);
	KUNIT_ASSERT_GT(test, rate, info->max_tmds_clock * 1000);

	rate = drm_hdmi_compute_mode_clock(preferred, 12, DRM_OUTPUT_COLOR_FORMAT_YCBCR422);
	KUNIT_ASSERT_LT(test, rate, info->max_tmds_clock * 1000);

	drm_modeset_acquire_init(&ctx, 0);

retry_conn_enable:
	ret = drm_kunit_helper_enable_crtc_connector(test, drm,
						     crtc, conn,
						     preferred,
						     &ctx);
	if (ret == -EDEADLK) {
		ret = drm_modeset_backoff(&ctx);
		if (!ret)
			goto retry_conn_enable;
	}
	KUNIT_EXPECT_EQ(test, ret, 0);

	conn_state = conn->state;
	KUNIT_ASSERT_NOT_NULL(test, conn_state);

	KUNIT_EXPECT_LT(test, conn_state->hdmi.output_bpc, 12);
	KUNIT_EXPECT_EQ(test, conn_state->hdmi.output_format, DRM_OUTPUT_COLOR_FORMAT_RGB444);

	drm_modeset_drop_locks(&ctx);
	drm_modeset_acquire_fini(&ctx);
}

/*
 * Test that if a display supports higher bpc but the driver only
 * supports 8 bpc, we only end up with 8 bpc even if we could have had a
 * higher bpc.
 */
static void drm_test_check_output_bpc_format_driver_8bpc_only(struct kunit *test)
{
	struct drm_atomic_helper_connector_hdmi_priv *priv;
	struct drm_modeset_acquire_ctx ctx;
	struct drm_connector_state *conn_state;
	struct drm_display_info *info;
	struct drm_display_mode *preferred;
	unsigned long long rate;
	struct drm_connector *conn;
	struct drm_device *drm;
	struct drm_crtc *crtc;
	int ret;

	priv = drm_kunit_helper_connector_hdmi_init_with_edid_funcs(test,
				&dummy_connector_hdmi_funcs,
				test_edid_hdmi_1080p_rgb_yuv_dc_max_340mhz);
	KUNIT_ASSERT_NOT_NULL(test, priv);

	drm = &priv->drm;
	crtc = priv->crtc;
	conn = &priv->connector;
	info = &conn->display_info;
	KUNIT_ASSERT_TRUE(test, info->is_hdmi);
	KUNIT_ASSERT_GT(test, info->max_tmds_clock, 0);

	preferred = find_preferred_mode(conn);
	KUNIT_ASSERT_NOT_NULL(test, preferred);

	/*
	 * We're making sure that we have headroom on the TMDS character
	 * clock to actually use 12bpc.
	 */
	rate = drm_hdmi_compute_mode_clock(preferred, 12, DRM_OUTPUT_COLOR_FORMAT_RGB444);
	KUNIT_ASSERT_LT(test, rate, info->max_tmds_clock * 1000);

	drm_modeset_acquire_init(&ctx, 0);

retry_conn_enable:
	ret = drm_kunit_helper_enable_crtc_connector(test, drm,
						     crtc, conn,
						     preferred,
						     &ctx);
	if (ret == -EDEADLK) {
		ret = drm_modeset_backoff(&ctx);
		if (!ret)
			goto retry_conn_enable;
	}
	KUNIT_EXPECT_EQ(test, ret, 0);

	conn_state = conn->state;
	KUNIT_ASSERT_NOT_NULL(test, conn_state);

	KUNIT_EXPECT_EQ(test, conn_state->hdmi.output_bpc, 8);
	KUNIT_EXPECT_EQ(test, conn_state->hdmi.output_format, DRM_OUTPUT_COLOR_FORMAT_RGB444);

	drm_modeset_drop_locks(&ctx);
	drm_modeset_acquire_fini(&ctx);
}

/*
 * Test that if a driver supports higher bpc but the display only
 * supports 8 bpc, we only end up with 8 bpc even if we could have had a
 * higher bpc.
 */
static void drm_test_check_output_bpc_format_display_8bpc_only(struct kunit *test)
{
	struct drm_connector_hdmi_funcs hdmi_funcs = dummy_connector_hdmi_funcs;
	struct drm_atomic_helper_connector_hdmi_priv *priv;
	struct drm_modeset_acquire_ctx ctx;
	struct drm_connector_state *conn_state;
	struct drm_display_info *info;
	struct drm_display_mode *preferred;
	unsigned long long rate;
	struct drm_connector *conn;
	struct drm_device *drm;
	struct drm_crtc *crtc;
	int ret;

	hdmi_funcs.max_bpc = 12;
	hdmi_funcs.supported_formats = BIT(DRM_OUTPUT_COLOR_FORMAT_RGB444) |
				       BIT(DRM_OUTPUT_COLOR_FORMAT_YCBCR422) |
				       BIT(DRM_OUTPUT_COLOR_FORMAT_YCBCR444);
	priv = drm_kunit_helper_connector_hdmi_init_with_edid_funcs(test,
				&hdmi_funcs,
				test_edid_hdmi_1080p_rgb_max_340mhz);
	KUNIT_ASSERT_NOT_NULL(test, priv);

	drm = &priv->drm;
	crtc = priv->crtc;
	conn = &priv->connector;
	info = &conn->display_info;
	KUNIT_ASSERT_TRUE(test, info->is_hdmi);
	KUNIT_ASSERT_GT(test, info->max_tmds_clock, 0);

	preferred = find_preferred_mode(conn);
	KUNIT_ASSERT_NOT_NULL(test, preferred);

	/*
	 * We're making sure that we have headroom on the TMDS character
	 * clock to actually use 12bpc.
	 */
	rate = drm_hdmi_compute_mode_clock(preferred, 12, DRM_OUTPUT_COLOR_FORMAT_RGB444);
	KUNIT_ASSERT_LT(test, rate, info->max_tmds_clock * 1000);

	drm_modeset_acquire_init(&ctx, 0);

retry_conn_enable:
	ret = drm_kunit_helper_enable_crtc_connector(test, drm,
						     crtc, conn,
						     preferred,
						     &ctx);
	if (ret == -EDEADLK) {
		ret = drm_modeset_backoff(&ctx);
		if (!ret)
			goto retry_conn_enable;
	}
	KUNIT_EXPECT_EQ(test, ret, 0);

	conn_state = conn->state;
	KUNIT_ASSERT_NOT_NULL(test, conn_state);

	KUNIT_EXPECT_EQ(test, conn_state->hdmi.output_bpc, 8);
	KUNIT_EXPECT_EQ(test, conn_state->hdmi.output_format, DRM_OUTPUT_COLOR_FORMAT_RGB444);

	drm_modeset_drop_locks(&ctx);
	drm_modeset_acquire_fini(&ctx);
}

/* Test that atomic check succeeds when disabling a connector. */
static void drm_test_check_disable_connector(struct kunit *test)
{
	struct drm_atomic_helper_connector_hdmi_priv *priv;
	struct drm_modeset_acquire_ctx ctx;
	struct drm_connector_state *conn_state;
	struct drm_crtc_state *crtc_state;
	struct drm_atomic_commit *state;
	struct drm_display_mode *preferred;
	struct drm_connector *conn;
	struct drm_device *drm;
	struct drm_crtc *crtc;
	int ret;

	priv = drm_kunit_helper_connector_hdmi_init(test);
	KUNIT_ASSERT_NOT_NULL(test, priv);

	drm_modeset_acquire_init(&ctx, 0);

	conn = &priv->connector;
	preferred = find_preferred_mode(conn);
	KUNIT_ASSERT_NOT_NULL(test, preferred);

	drm = &priv->drm;
	crtc = priv->crtc;

retry_conn_enable:
	ret = drm_kunit_helper_enable_crtc_connector(test, drm,
						     crtc, conn,
						     preferred,
						     &ctx);
	if (ret == -EDEADLK) {
		ret = drm_modeset_backoff(&ctx);
		if (!ret)
			goto retry_conn_enable;
	}
	KUNIT_ASSERT_EQ(test, ret, 0);

	state = drm_kunit_helper_atomic_state_alloc(test, drm, &ctx);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, state);

	crtc_state = drm_atomic_get_crtc_state(state, crtc);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, crtc_state);

	crtc_state->active = false;
	ret = drm_atomic_set_mode_for_crtc(crtc_state, NULL);
	KUNIT_EXPECT_EQ(test, ret, 0);

	conn_state = drm_atomic_get_connector_state(state, conn);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, conn_state);

	ret = drm_atomic_set_crtc_for_connector(conn_state, NULL);
	KUNIT_EXPECT_EQ(test, ret, 0);

	ret = drm_atomic_check_only(state);
	KUNIT_ASSERT_EQ(test, ret, 0);

	drm_modeset_drop_locks(&ctx);
	drm_modeset_acquire_fini(&ctx);
}

/*
 * Test that on an HDMI connector with source+sink scrambling support, driving
 * a sub-340 MHz CEA mode, we end up with scrambler_needed cleared.
 */
static void drm_test_check_scrambler_needed_low_rate(struct kunit *test)
{
	struct drm_atomic_helper_connector_hdmi_priv *priv;
	struct drm_modeset_acquire_ctx ctx;
	struct drm_connector_state *conn_state;
	struct drm_display_info *info;
	struct drm_display_mode *low_rate_mode;
	struct drm_connector *conn;
	struct drm_device *drm;
	struct drm_crtc *crtc;
	unsigned long long rate;
	int ret;

	priv = drm_kunit_helper_connector_hdmi_init_with_edid_funcs(test,
				&scrambler_connector_hdmi_funcs,
				test_edid_hdmi_4k_rgb_yuv420_dc_max_600mhz);
	KUNIT_ASSERT_NOT_NULL(test, priv);

	drm = &priv->drm;
	crtc = priv->crtc;
	conn = &priv->connector;
	info = &conn->display_info;
	KUNIT_ASSERT_TRUE(test, drm_connector_hdmi_scrambler_supported(conn));
	KUNIT_ASSERT_TRUE(test, info->is_hdmi);
	KUNIT_ASSERT_TRUE(test, info->hdmi.scdc.supported);
	KUNIT_ASSERT_TRUE(test, info->hdmi.scdc.scrambling.supported);

	low_rate_mode = drm_kunit_display_mode_from_cea_vic(test, drm, 16);
	KUNIT_ASSERT_NOT_NULL(test, low_rate_mode);

	rate = drm_hdmi_compute_mode_clock(low_rate_mode, 8, DRM_OUTPUT_COLOR_FORMAT_RGB444);
	KUNIT_ASSERT_LT(test, rate, HDMI_1_3_TMDS_CHAR_RATE_MAX_HZ);

	drm_modeset_acquire_init(&ctx, 0);

retry_conn_enable:
	ret = drm_kunit_helper_enable_crtc_connector(test, drm, crtc, conn,
						     low_rate_mode, &ctx);
	if (ret == -EDEADLK) {
		ret = drm_modeset_backoff(&ctx);
		if (!ret)
			goto retry_conn_enable;
	}
	KUNIT_ASSERT_EQ(test, ret, 0);

	conn_state = conn->state;
	KUNIT_ASSERT_NOT_NULL(test, conn_state);

	KUNIT_EXPECT_LE(test, conn_state->hdmi.tmds_char_rate,
			HDMI_1_3_TMDS_CHAR_RATE_MAX_HZ);
	KUNIT_EXPECT_FALSE(test, conn_state->hdmi.scrambler_needed);

	drm_modeset_drop_locks(&ctx);
	drm_modeset_acquire_fini(&ctx);
}

/*
 * Test that on an HDMI connector with source+sink scrambling support, driving
 * the 4K@60 RGB preferred mode (~594 MHz TMDS, above the 340 MHz threshold),
 * we end up with scrambler_needed set.
 */
static void drm_test_check_scrambler_needed_high_rate(struct kunit *test)
{
	struct drm_atomic_helper_connector_hdmi_priv *priv;
	struct drm_modeset_acquire_ctx ctx;
	struct drm_connector_state *conn_state;
	struct drm_display_info *info;
	struct drm_display_mode *preferred;
	struct drm_connector *conn;
	struct drm_device *drm;
	struct drm_crtc *crtc;
	unsigned long long rate;
	int ret;

	priv = drm_kunit_helper_connector_hdmi_init_with_edid_funcs(test,
				&scrambler_connector_hdmi_funcs,
				test_edid_hdmi_4k_rgb_yuv420_dc_max_600mhz);
	KUNIT_ASSERT_NOT_NULL(test, priv);

	drm = &priv->drm;
	crtc = priv->crtc;
	conn = &priv->connector;
	info = &conn->display_info;
	KUNIT_ASSERT_TRUE(test, drm_connector_hdmi_scrambler_supported(conn));
	KUNIT_ASSERT_TRUE(test, info->is_hdmi);
	KUNIT_ASSERT_TRUE(test, info->hdmi.scdc.supported);
	KUNIT_ASSERT_TRUE(test, info->hdmi.scdc.scrambling.supported);

	preferred = find_preferred_mode(conn);
	KUNIT_ASSERT_NOT_NULL(test, preferred);

	rate = drm_hdmi_compute_mode_clock(preferred, 8, DRM_OUTPUT_COLOR_FORMAT_RGB444);
	KUNIT_ASSERT_GT(test, rate, HDMI_1_3_TMDS_CHAR_RATE_MAX_HZ);

	drm_modeset_acquire_init(&ctx, 0);

retry_conn_enable:
	ret = drm_kunit_helper_enable_crtc_connector(test, drm, crtc, conn,
						     preferred, &ctx);
	if (ret == -EDEADLK) {
		ret = drm_modeset_backoff(&ctx);
		if (!ret)
			goto retry_conn_enable;
	}
	KUNIT_ASSERT_EQ(test, ret, 0);

	conn_state = conn->state;
	KUNIT_ASSERT_NOT_NULL(test, conn_state);

	KUNIT_EXPECT_GT(test, conn_state->hdmi.tmds_char_rate,
			HDMI_1_3_TMDS_CHAR_RATE_MAX_HZ);
	KUNIT_EXPECT_TRUE(test, conn_state->hdmi.scrambler_needed);

	drm_modeset_drop_locks(&ctx);
	drm_modeset_acquire_fini(&ctx);
}

/*
 * Test that if:
 * - We have an HDMI connector supporting RGB only and up to 12 bpc
 * - The chosen mode has a TMDS character rate higher than the display
 *   supports in RGB/12bpc
 * - The display supports HDMI 2.1 FRL with enough bandwidth
 * - The driver provides a .frl_rate_valid callback that allows the bandwidth.
 *
 * Then the mode is accepted at RGB/12bpc and the TMDS clock rate stored
 * in the connector state is equal to 1.5 times the mode pixel clock.
 */
static void drm_test_check_frl_rate_rgb_12bpc(struct kunit *test)
{
	struct drm_connector_hdmi_funcs hdmi_funcs = accept_frl_connector_hdmi_funcs;
	struct drm_atomic_helper_connector_hdmi_priv *priv;
	struct drm_modeset_acquire_ctx ctx;
	struct drm_connector_state *conn_state;
	struct drm_display_info *info;
	struct drm_display_mode *preferred;
	unsigned long long rate;
	struct drm_connector *conn;
	struct drm_device *drm;
	struct drm_crtc *crtc;
	int ret;

	hdmi_funcs.max_bpc = 12;
	priv = drm_kunit_helper_connector_hdmi_init_with_edid_funcs(test,
				&hdmi_funcs,
				test_edid_hdmi_4k_rgb_yuv420_dc_max_600mhz_frl_48gbps);
	KUNIT_ASSERT_NOT_NULL(test, priv);

	drm = &priv->drm;
	crtc = priv->crtc;
	conn = &priv->connector;
	info = &conn->display_info;
	KUNIT_ASSERT_TRUE(test, info->is_hdmi);
	KUNIT_ASSERT_GT(test, info->max_tmds_clock, 0);
	KUNIT_ASSERT_GT(test, info->hdmi.max_frl_rate_per_lane, 0);
	KUNIT_ASSERT_GT(test, info->hdmi.max_lanes, 0);

	preferred = find_preferred_mode(conn);
	KUNIT_ASSERT_NOT_NULL(test, preferred);
	KUNIT_ASSERT_FALSE(test, preferred->flags & DRM_MODE_FLAG_DBLCLK);

	rate = drm_hdmi_compute_mode_clock(preferred, 12, DRM_OUTPUT_COLOR_FORMAT_RGB444);
	KUNIT_ASSERT_GT(test, rate, info->max_tmds_clock * 1000);

	drm_modeset_acquire_init(&ctx, 0);

retry_conn_enable:
	ret = drm_kunit_helper_enable_crtc_connector(test, drm,
						     crtc, conn,
						     preferred,
						     &ctx);
	if (ret == -EDEADLK) {
		ret = drm_modeset_backoff(&ctx);
		if (!ret)
			goto retry_conn_enable;
	}
	KUNIT_EXPECT_EQ(test, ret, 0);

	conn_state = conn->state;
	KUNIT_ASSERT_NOT_NULL(test, conn_state);

	KUNIT_EXPECT_EQ(test, conn_state->hdmi.output_bpc, 12);
	KUNIT_EXPECT_EQ(test, conn_state->hdmi.output_format, DRM_OUTPUT_COLOR_FORMAT_RGB444);
	KUNIT_EXPECT_EQ(test, conn_state->hdmi.tmds_char_rate, preferred->clock * 1500);

	drm_modeset_drop_locks(&ctx);
	drm_modeset_acquire_fini(&ctx);
}

/*
 * Test that if:
 * - We have an HDMI connector supporting RGB only and up to 12 bpc
 * - The chosen mode has a TMDS character rate higher than the display
 *   supports in RGB/12bpc
 * - The display supports HDMI 2.1 FRL with enough bandwidth
 * - The driver does NOT provide a .frl_rate_valid callback.
 *
 * Then the mode is still accepted at RGB/12bpc, since that callback only
 * narrows down the FRL rates the source and the sink already agree upon,
 * and the highest of those rates is selected.
 */
static void drm_test_check_frl_no_rate_valid_func(struct kunit *test)
{
	struct drm_connector_hdmi_funcs hdmi_funcs = accept_frl_connector_hdmi_funcs;
	struct drm_atomic_helper_connector_hdmi_priv *priv;
	struct drm_modeset_acquire_ctx ctx;
	struct drm_connector_state *conn_state;
	struct drm_display_info *info;
	struct drm_display_mode *preferred;
	unsigned long long rate;
	struct drm_connector *conn;
	struct drm_device *drm;
	struct drm_crtc *crtc;
	int ret;

	hdmi_funcs.max_bpc = 12;
	hdmi_funcs.frl_rate_valid = NULL;
	priv = drm_kunit_helper_connector_hdmi_init_with_edid_funcs(test,
				&hdmi_funcs,
				test_edid_hdmi_4k_rgb_yuv420_dc_max_600mhz_frl_48gbps);
	KUNIT_ASSERT_NOT_NULL(test, priv);

	drm = &priv->drm;
	crtc = priv->crtc;
	conn = &priv->connector;
	info = &conn->display_info;
	KUNIT_ASSERT_TRUE(test, info->is_hdmi);
	KUNIT_ASSERT_GT(test, info->max_tmds_clock, 0);
	KUNIT_ASSERT_GT(test, info->hdmi.max_frl_rate_per_lane, 0);
	KUNIT_ASSERT_GT(test, info->hdmi.max_lanes, 0);

	preferred = find_preferred_mode(conn);
	KUNIT_ASSERT_NOT_NULL(test, preferred);
	KUNIT_ASSERT_FALSE(test, preferred->flags & DRM_MODE_FLAG_DBLCLK);

	rate = drm_hdmi_compute_mode_clock(preferred, 12, DRM_OUTPUT_COLOR_FORMAT_RGB444);
	KUNIT_ASSERT_GT(test, rate, info->max_tmds_clock * 1000);

	rate = drm_hdmi_compute_mode_clock(preferred, 8, DRM_OUTPUT_COLOR_FORMAT_RGB444);
	KUNIT_ASSERT_LT(test, rate, info->max_tmds_clock * 1000);

	drm_modeset_acquire_init(&ctx, 0);

retry_conn_enable:
	ret = drm_kunit_helper_enable_crtc_connector(test, drm,
						     crtc, conn,
						     preferred,
						     &ctx);
	if (ret == -EDEADLK) {
		ret = drm_modeset_backoff(&ctx);
		if (!ret)
			goto retry_conn_enable;
	}
	KUNIT_EXPECT_EQ(test, ret, 0);

	conn_state = conn->state;
	KUNIT_ASSERT_NOT_NULL(test, conn_state);

	KUNIT_EXPECT_EQ(test, conn_state->hdmi.output_bpc, 12);
	KUNIT_EXPECT_EQ(test, conn_state->hdmi.output_format, DRM_OUTPUT_COLOR_FORMAT_RGB444);
	KUNIT_EXPECT_EQ(test, conn_state->hdmi.frl_rate_per_lane, 12);
	KUNIT_EXPECT_EQ(test, conn_state->hdmi.frl_lanes, 4);
	KUNIT_EXPECT_FALSE(test, conn_state->hdmi.scrambler_needed);

	drm_modeset_drop_locks(&ctx);
	drm_modeset_acquire_fini(&ctx);
}

/*
 * Test that if:
 * - We have an HDMI connector supporting RGB only and up to 12 bpc
 * - The chosen mode has a TMDS character rate higher than the display
 *   supports in RGB/12bpc
 * - The display supports HDMI 2.1 FRL with enough bandwidth
 * - The driver provides a .frl_rate_valid callback that doesn't accept
 *   the bandwidth
 *
 * Then the FRL path is rejected by the driver and the mode falls back
 * to RGB/8bpc.
 */
static void drm_test_check_frl_reject_rate(struct kunit *test)
{
	struct drm_connector_hdmi_funcs hdmi_funcs = reject_frl_connector_hdmi_funcs;
	struct drm_atomic_helper_connector_hdmi_priv *priv;
	struct drm_modeset_acquire_ctx ctx;
	struct drm_connector_state *conn_state;
	struct drm_display_info *info;
	struct drm_display_mode *preferred;
	unsigned long long rate;
	struct drm_connector *conn;
	struct drm_device *drm;
	struct drm_crtc *crtc;
	int ret;

	hdmi_funcs.max_bpc = 12;
	priv = drm_kunit_helper_connector_hdmi_init_with_edid_funcs(test,
				&hdmi_funcs,
				test_edid_hdmi_4k_rgb_yuv420_dc_max_600mhz_frl_48gbps);
	KUNIT_ASSERT_NOT_NULL(test, priv);

	drm = &priv->drm;
	crtc = priv->crtc;
	conn = &priv->connector;
	info = &conn->display_info;
	KUNIT_ASSERT_TRUE(test, info->is_hdmi);
	KUNIT_ASSERT_GT(test, info->max_tmds_clock, 0);
	KUNIT_ASSERT_GT(test, info->hdmi.max_frl_rate_per_lane, 0);
	KUNIT_ASSERT_GT(test, info->hdmi.max_lanes, 0);

	preferred = find_preferred_mode(conn);
	KUNIT_ASSERT_NOT_NULL(test, preferred);
	KUNIT_ASSERT_FALSE(test, preferred->flags & DRM_MODE_FLAG_DBLCLK);

	rate = drm_hdmi_compute_mode_clock(preferred, 12, DRM_OUTPUT_COLOR_FORMAT_RGB444);
	KUNIT_ASSERT_GT(test, rate, info->max_tmds_clock * 1000);

	rate = drm_hdmi_compute_mode_clock(preferred, 8, DRM_OUTPUT_COLOR_FORMAT_RGB444);
	KUNIT_ASSERT_LT(test, rate, info->max_tmds_clock * 1000);

	drm_modeset_acquire_init(&ctx, 0);

retry_conn_enable:
	ret = drm_kunit_helper_enable_crtc_connector(test, drm,
						     crtc, conn,
						     preferred,
						     &ctx);
	if (ret == -EDEADLK) {
		ret = drm_modeset_backoff(&ctx);
		if (!ret)
			goto retry_conn_enable;
	}
	KUNIT_EXPECT_EQ(test, ret, 0);

	conn_state = conn->state;
	KUNIT_ASSERT_NOT_NULL(test, conn_state);

	KUNIT_EXPECT_EQ(test, conn_state->hdmi.output_bpc, 8);
	KUNIT_EXPECT_EQ(test, conn_state->hdmi.output_format, DRM_OUTPUT_COLOR_FORMAT_RGB444);

	drm_modeset_drop_locks(&ctx);
	drm_modeset_acquire_fini(&ctx);
}

struct color_format_test_param {
	enum drm_connector_color_format fmt;
	enum drm_output_color_format expected;
	int expected_ret;
	const char *desc;
};

/* Test that if:
 * - an HDMI connector supports RGB, YUV444, YUV422, and YUV420
 * - the display supports RGB, YUV444, YUV422, and YUV420
 * - the "color format" property is set
 * then, for the preferred mode, for a given "color format" option:
 * - DRM_CONNECTOR_COLOR_FORMAT_AUTO results in an output format of RGB
 * - DRM_CONNECTOR_COLOR_FORMAT_YCBCR422 results in an output format of YUV422
 * - DRM_CONNECTOR_COLOR_FORMAT_YCBCR420 results in an output format of YUV420
 * - DRM_CONNECTOR_COLOR_FORMAT_YCBCR444 results in an output format of YUV444
 * - DRM_CONNECTOR_COLOR_FORMAT_RGB results in an HDMI output format of RGB
 */
static void drm_test_check_hdmi_color_format(struct kunit *test)
{
	struct drm_connector_hdmi_funcs hdmi_funcs = dummy_connector_hdmi_funcs;
	const struct color_format_test_param *param = test->param_value;
	struct drm_atomic_helper_connector_hdmi_priv *priv;
	struct drm_connector_state *conn_state;
	struct drm_modeset_acquire_ctx ctx;
	struct drm_crtc_state *crtc_state;
	struct drm_atomic_commit *state;
	struct drm_display_info *info;
	struct drm_display_mode *preferred;
	int ret;

	hdmi_funcs.max_bpc = 12;
	hdmi_funcs.supported_formats = BIT(DRM_OUTPUT_COLOR_FORMAT_RGB444) |
				       BIT(DRM_OUTPUT_COLOR_FORMAT_YCBCR422) |
				       BIT(DRM_OUTPUT_COLOR_FORMAT_YCBCR420) |
				       BIT(DRM_OUTPUT_COLOR_FORMAT_YCBCR444);
	priv = drm_kunit_helper_connector_hdmi_init_with_edid_funcs(test,
				&hdmi_funcs,
				test_edid_hdmi_4k_rgb_yuv420_dc_max_340mhz);
	KUNIT_ASSERT_NOT_NULL(test, priv);

	drm_modeset_acquire_init(&ctx, 0);

	KUNIT_ASSERT_TRUE(test, priv->connector.ycbcr_420_allowed);

	info = &priv->connector.display_info;
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, info);
	preferred = find_preferred_mode(&priv->connector);
	KUNIT_ASSERT_TRUE(test, drm_mode_is_420(info, preferred));

	state = drm_kunit_helper_atomic_state_alloc(test, &priv->drm, &ctx);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, state);

	conn_state = drm_atomic_get_connector_state(state, &priv->connector);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, conn_state);

	conn_state->color_format = param->fmt;

	ret = drm_atomic_set_crtc_for_connector(conn_state, priv->crtc);
	KUNIT_ASSERT_EQ(test, ret, 0);

	crtc_state = drm_atomic_get_crtc_state(state, priv->crtc);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, crtc_state);

	ret = drm_atomic_set_mode_for_crtc(crtc_state, preferred);
	KUNIT_ASSERT_EQ(test, ret, 0);

	crtc_state->enable = true;
	crtc_state->active = true;

	ret = drm_atomic_check_only(state);
	KUNIT_EXPECT_EQ(test, ret, param->expected_ret);
	KUNIT_EXPECT_EQ(test, conn_state->hdmi.output_format, param->expected);

	drm_modeset_drop_locks(&ctx);
	drm_modeset_acquire_fini(&ctx);
}

static const struct color_format_test_param hdmi_color_format_params[] = {
	{
		.fmt = DRM_CONNECTOR_COLOR_FORMAT_AUTO,
		.expected = DRM_OUTPUT_COLOR_FORMAT_RGB444,
		.expected_ret = 0,
		.desc = "AUTO -> RGB"
	},
	{
		.fmt = DRM_CONNECTOR_COLOR_FORMAT_YCBCR422,
		.expected = DRM_OUTPUT_COLOR_FORMAT_YCBCR422,
		.expected_ret = 0,
		.desc = "YCBCR422 -> YUV422"
	},
	{
		.fmt = DRM_CONNECTOR_COLOR_FORMAT_YCBCR420,
		.expected = DRM_OUTPUT_COLOR_FORMAT_YCBCR420,
		.expected_ret = 0,
		.desc = "YCBCR420 -> YUV420"
	},
	{
		.fmt = DRM_CONNECTOR_COLOR_FORMAT_YCBCR444,
		.expected = DRM_OUTPUT_COLOR_FORMAT_YCBCR444,
		.expected_ret = 0,
		.desc = "YCBCR444 -> YUV444"
	},
	{
		.fmt = DRM_CONNECTOR_COLOR_FORMAT_RGB444,
		.expected = DRM_OUTPUT_COLOR_FORMAT_RGB444,
		.expected_ret = 0,
		.desc = "RGB -> RGB"
	},
};

KUNIT_ARRAY_PARAM_DESC(check_hdmi_color_format, hdmi_color_format_params, desc);

/* Test that if:
 * - the HDMI connector supports RGB, YUV422, YUV420, and YUV444
 * - the display has a YUV420-only mode
 * - the "color format" property is explicitly set (i.e. !AUTO)
 * then:
 * - color format DRM_CONNECTOR_COLOR_FORMAT_RGB444 will fail
 *   drm_atomic_check_only for the YUV420-only mode with -EINVAL
 * - color format DRM_CONNECTOR_COLOR_FORMAT_YCBCR444 will fail
 *   drm_atomic_check_only for the YUV420-only mode with -EINVAL
 * - color format DRM_CONNECTOR_COLOR_FORMAT_YCBCR422 will fail
 *   drm_atomic_check_only for the YUV420-only mode with -EINVAL
 * - color format DRM_CONNECTOR_COLOR_FORMAT_YCBCR420 passes
 *   drm_atomic_check_only for the YUV420-only mode
 */
static void drm_test_check_hdmi_color_format_420_only(struct kunit *test)
{
	struct drm_connector_hdmi_funcs hdmi_funcs = dummy_connector_hdmi_funcs;
	const struct color_format_test_param *param = test->param_value;
	struct drm_atomic_helper_connector_hdmi_priv *priv;
	struct drm_connector_state *conn_state;
	struct drm_modeset_acquire_ctx ctx;
	struct drm_crtc_state *crtc_state;
	struct drm_atomic_commit *state;
	struct drm_display_mode *dank;
	int ret;

	hdmi_funcs.max_bpc = 12;
	hdmi_funcs.supported_formats = BIT(DRM_OUTPUT_COLOR_FORMAT_RGB444) |
				       BIT(DRM_OUTPUT_COLOR_FORMAT_YCBCR422) |
				       BIT(DRM_OUTPUT_COLOR_FORMAT_YCBCR420) |
				       BIT(DRM_OUTPUT_COLOR_FORMAT_YCBCR444);
	priv = drm_kunit_helper_connector_hdmi_init_with_edid_funcs(test,
				&hdmi_funcs,
				test_edid_hdmi_1080p_rgb_yuv_4k_yuv420_dc_max_200mhz);
	KUNIT_ASSERT_NOT_NULL(test, priv);

	drm_modeset_acquire_init(&ctx, 0);

	dank = find_420_only_mode(&priv->connector);
	KUNIT_ASSERT_NOT_NULL(test, dank);

	state = drm_kunit_helper_atomic_state_alloc(test, &priv->drm, &ctx);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, state);

	conn_state = drm_atomic_get_connector_state(state, &priv->connector);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, conn_state);

	conn_state->color_format = param->fmt;

	ret = drm_atomic_set_crtc_for_connector(conn_state, priv->crtc);
	KUNIT_ASSERT_EQ(test, ret, 0);

	crtc_state = drm_atomic_get_crtc_state(state, priv->crtc);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, crtc_state);

	ret = drm_atomic_set_mode_for_crtc(crtc_state, dank);
	KUNIT_ASSERT_EQ(test, ret, 0);

	crtc_state->enable = true;
	crtc_state->active = true;

	ret = drm_atomic_check_only(state);
	KUNIT_EXPECT_EQ(test, ret, param->expected_ret);
	if (!param->expected_ret)
		KUNIT_EXPECT_EQ(test, conn_state->hdmi.output_format, param->expected);

	drm_modeset_drop_locks(&ctx);
	drm_modeset_acquire_fini(&ctx);
};

static const struct color_format_test_param hdmi_color_format_420_only_params[] = {
	{
		.fmt = DRM_CONNECTOR_COLOR_FORMAT_RGB444,
		.expected = DRM_OUTPUT_COLOR_FORMAT_RGB444,
		.expected_ret = -EINVAL,
		.desc = "RGB should fail"
	},
	{
		.fmt = DRM_CONNECTOR_COLOR_FORMAT_YCBCR444,
		.expected = DRM_OUTPUT_COLOR_FORMAT_YCBCR444,
		.expected_ret = -EINVAL,
		.desc = "YUV444 should fail"
	},
	{
		.fmt = DRM_CONNECTOR_COLOR_FORMAT_YCBCR422,
		.expected = DRM_OUTPUT_COLOR_FORMAT_YCBCR422,
		.expected_ret = -EINVAL,
		.desc = "YUV422 should fail"
	},
	{
		.fmt = DRM_CONNECTOR_COLOR_FORMAT_YCBCR420,
		.expected = DRM_OUTPUT_COLOR_FORMAT_YCBCR420,
		.expected_ret = 0,
		.desc = "YUV420 should work"
	},
};

KUNIT_ARRAY_PARAM_DESC(check_hdmi_color_format_420_only,
		       hdmi_color_format_420_only_params, desc);

static struct kunit_case drm_atomic_helper_connector_hdmi_check_tests[] = {
	KUNIT_CASE(drm_test_check_broadcast_rgb_auto_cea_mode),
	KUNIT_CASE(drm_test_check_broadcast_rgb_auto_cea_mode_vic_1),
	KUNIT_CASE(drm_test_check_broadcast_rgb_full_cea_mode),
	KUNIT_CASE(drm_test_check_broadcast_rgb_full_cea_mode_vic_1),
	KUNIT_CASE(drm_test_check_broadcast_rgb_limited_cea_mode),
	KUNIT_CASE(drm_test_check_broadcast_rgb_limited_cea_mode_vic_1),
	KUNIT_CASE_PARAM(drm_test_check_broadcast_rgb_cea_mode_yuv420,
			 check_broadcast_rgb_cea_mode_yuv420_gen_params),
	KUNIT_CASE(drm_test_check_broadcast_rgb_crtc_mode_changed),
	KUNIT_CASE(drm_test_check_broadcast_rgb_crtc_mode_not_changed),
	KUNIT_CASE(drm_test_check_disable_connector),
	KUNIT_CASE(drm_test_check_hdmi_funcs_reject_rate),
	KUNIT_CASE(drm_test_check_max_tmds_rate_bpc_fallback_rgb),
	KUNIT_CASE(drm_test_check_max_tmds_rate_bpc_fallback_yuv420),
	KUNIT_CASE(drm_test_check_max_tmds_rate_bpc_fallback_ignore_yuv422),
	KUNIT_CASE(drm_test_check_max_tmds_rate_bpc_fallback_ignore_yuv420),
	KUNIT_CASE(drm_test_check_driver_unsupported_fallback_yuv420),
	KUNIT_CASE(drm_test_check_output_bpc_crtc_mode_changed),
	KUNIT_CASE(drm_test_check_output_bpc_crtc_mode_not_changed),
	KUNIT_CASE(drm_test_check_output_bpc_dvi),
	KUNIT_CASE(drm_test_check_output_bpc_format_vic_1),
	KUNIT_CASE(drm_test_check_output_bpc_format_display_8bpc_only),
	KUNIT_CASE(drm_test_check_output_bpc_format_display_rgb_only),
	KUNIT_CASE(drm_test_check_output_bpc_format_driver_8bpc_only),
	KUNIT_CASE(drm_test_check_output_bpc_format_driver_rgb_only),
	KUNIT_CASE(drm_test_check_tmds_char_rate_rgb_8bpc),
	KUNIT_CASE(drm_test_check_tmds_char_rate_rgb_10bpc),
	KUNIT_CASE(drm_test_check_tmds_char_rate_rgb_12bpc),
	KUNIT_CASE(drm_test_check_scrambler_needed_low_rate),
	KUNIT_CASE(drm_test_check_scrambler_needed_high_rate),
	KUNIT_CASE(drm_test_check_frl_rate_rgb_12bpc),
	KUNIT_CASE(drm_test_check_frl_no_rate_valid_func),
	KUNIT_CASE(drm_test_check_frl_reject_rate),
	KUNIT_CASE_PARAM(drm_test_check_hdmi_color_format,
			 check_hdmi_color_format_gen_params),
	KUNIT_CASE_PARAM(drm_test_check_hdmi_color_format_420_only,
			 check_hdmi_color_format_420_only_gen_params),
	/*
	 * TODO: We should have tests to check that a change in the
	 * format triggers a CRTC mode change just like we do for the
	 * RGB Quantization and BPC.
	 *
	 * However, we don't have any way to control which format gets
	 * picked up aside from changing the BPC or mode which would
	 * already trigger a mode change.
	 */
	{ }
};

static struct kunit_suite drm_atomic_helper_connector_hdmi_check_test_suite = {
	.name		= "drm_atomic_helper_connector_hdmi_check",
	.test_cases	= drm_atomic_helper_connector_hdmi_check_tests,
};

/*
 * Test that the value of the Broadcast RGB property out of reset is set
 * to auto.
 */
static void drm_test_check_broadcast_rgb_value(struct kunit *test)
{
	struct drm_atomic_helper_connector_hdmi_priv *priv;
	struct drm_connector_state *conn_state;
	struct drm_connector *conn;

	priv = drm_kunit_helper_connector_hdmi_init(test);
	KUNIT_ASSERT_NOT_NULL(test, priv);

	conn = &priv->connector;
	conn_state = conn->state;
	KUNIT_EXPECT_EQ(test, conn_state->hdmi.broadcast_rgb, DRM_HDMI_BROADCAST_RGB_AUTO);
}

/*
 * Test that if the connector was initialised with a maximum bpc of 8,
 * the value of the max_bpc and max_requested_bpc properties out of
 * reset are also set to 8, and output_bpc is set to 0 and will be
 * filled at atomic_check time.
 */
static void drm_test_check_bpc_8_value(struct kunit *test)
{
	struct drm_atomic_helper_connector_hdmi_priv *priv;
	struct drm_connector_state *conn_state;
	struct drm_connector *conn;

	priv = drm_kunit_helper_connector_hdmi_init(test);
	KUNIT_ASSERT_NOT_NULL(test, priv);

	conn = &priv->connector;
	conn_state = conn->state;
	KUNIT_EXPECT_EQ(test, conn_state->max_bpc, 8);
	KUNIT_EXPECT_EQ(test, conn_state->max_requested_bpc, 8);
	KUNIT_EXPECT_EQ(test, conn_state->hdmi.output_bpc, 0);
}

/*
 * Test that if the connector was initialised with a maximum bpc of 10,
 * the value of the max_bpc and max_requested_bpc properties out of
 * reset are also set to 10, and output_bpc is set to 0 and will be
 * filled at atomic_check time.
 */
static void drm_test_check_bpc_10_value(struct kunit *test)
{
	struct drm_connector_hdmi_funcs hdmi_funcs = dummy_connector_hdmi_funcs;
	struct drm_atomic_helper_connector_hdmi_priv *priv;
	struct drm_connector_state *conn_state;
	struct drm_connector *conn;

	hdmi_funcs.max_bpc = 10;
	priv = drm_kunit_helper_connector_hdmi_init_with_edid_funcs(test,
				&hdmi_funcs,
				test_edid_hdmi_1080p_rgb_max_200mhz);
	KUNIT_ASSERT_NOT_NULL(test, priv);

	conn = &priv->connector;
	conn_state = conn->state;
	KUNIT_EXPECT_EQ(test, conn_state->max_bpc, 10);
	KUNIT_EXPECT_EQ(test, conn_state->max_requested_bpc, 10);
	KUNIT_EXPECT_EQ(test, conn_state->hdmi.output_bpc, 0);
}

/*
 * Test that if the connector was initialised with a maximum bpc of 12,
 * the value of the max_bpc and max_requested_bpc properties out of
 * reset are also set to 12, and output_bpc is set to 0 and will be
 * filled at atomic_check time.
 */
static void drm_test_check_bpc_12_value(struct kunit *test)
{
	struct drm_connector_hdmi_funcs hdmi_funcs = dummy_connector_hdmi_funcs;
	struct drm_atomic_helper_connector_hdmi_priv *priv;
	struct drm_connector_state *conn_state;
	struct drm_connector *conn;

	hdmi_funcs.max_bpc = 12;
	priv = drm_kunit_helper_connector_hdmi_init_with_edid_funcs(test,
				&hdmi_funcs,
				test_edid_hdmi_1080p_rgb_max_200mhz);
	KUNIT_ASSERT_NOT_NULL(test, priv);

	conn = &priv->connector;
	conn_state = conn->state;
	KUNIT_EXPECT_EQ(test, conn_state->max_bpc, 12);
	KUNIT_EXPECT_EQ(test, conn_state->max_requested_bpc, 12);
	KUNIT_EXPECT_EQ(test, conn_state->hdmi.output_bpc, 0);
}

/*
 * Test that the value of the output format property out of reset is set
 * to RGB, even if the driver supports more than that.
 */
static void drm_test_check_format_value(struct kunit *test)
{
	struct drm_connector_hdmi_funcs hdmi_funcs = dummy_connector_hdmi_funcs;
	struct drm_atomic_helper_connector_hdmi_priv *priv;
	struct drm_connector_state *conn_state;
	struct drm_connector *conn;

	hdmi_funcs.supported_formats = BIT(DRM_OUTPUT_COLOR_FORMAT_RGB444) |
				       BIT(DRM_OUTPUT_COLOR_FORMAT_YCBCR422) |
				       BIT(DRM_OUTPUT_COLOR_FORMAT_YCBCR444);
	priv = drm_kunit_helper_connector_hdmi_init_with_edid_funcs(test,
				&hdmi_funcs,
				test_edid_hdmi_1080p_rgb_max_200mhz);
	KUNIT_ASSERT_NOT_NULL(test, priv);

	conn = &priv->connector;
	conn_state = conn->state;
	KUNIT_EXPECT_EQ(test, conn_state->hdmi.output_format, 0);
}

/*
 * Test that the value of the output format property out of reset is set
 * to 0, and will be computed at atomic_check time.
 */
static void drm_test_check_tmds_char_value(struct kunit *test)
{
	struct drm_connector_hdmi_funcs hdmi_funcs = dummy_connector_hdmi_funcs;
	struct drm_atomic_helper_connector_hdmi_priv *priv;
	struct drm_connector_state *conn_state;
	struct drm_connector *conn;

	hdmi_funcs.max_bpc = 12;
	hdmi_funcs.supported_formats = BIT(DRM_OUTPUT_COLOR_FORMAT_RGB444) |
				       BIT(DRM_OUTPUT_COLOR_FORMAT_YCBCR422) |
				       BIT(DRM_OUTPUT_COLOR_FORMAT_YCBCR444);
	priv = drm_kunit_helper_connector_hdmi_init_with_edid_funcs(test,
				&hdmi_funcs,
				test_edid_hdmi_1080p_rgb_max_200mhz);
	KUNIT_ASSERT_NOT_NULL(test, priv);

	conn = &priv->connector;
	conn_state = conn->state;
	KUNIT_EXPECT_EQ(test, conn_state->hdmi.tmds_char_rate, 0);
}

static struct kunit_case drm_atomic_helper_connector_hdmi_reset_tests[] = {
	KUNIT_CASE(drm_test_check_broadcast_rgb_value),
	KUNIT_CASE(drm_test_check_bpc_8_value),
	KUNIT_CASE(drm_test_check_bpc_10_value),
	KUNIT_CASE(drm_test_check_bpc_12_value),
	KUNIT_CASE(drm_test_check_format_value),
	KUNIT_CASE(drm_test_check_tmds_char_value),
	{ }
};

static struct kunit_suite drm_atomic_helper_connector_hdmi_reset_test_suite = {
	.name		= "drm_atomic_helper_connector_hdmi_reset",
	.test_cases	= drm_atomic_helper_connector_hdmi_reset_tests,
};

/*
 * Test that the default behaviour for drm_hdmi_connector_mode_valid() is not
 * to reject any modes. Pass a correct EDID and verify that preferred mode
 * matches the expectations (1080p).
 */
static void drm_test_check_mode_valid(struct kunit *test)
{
	struct drm_atomic_helper_connector_hdmi_priv *priv;
	struct drm_connector *conn;
	struct drm_display_mode *preferred;

	priv = drm_kunit_helper_connector_hdmi_init(test);
	KUNIT_ASSERT_NOT_NULL(test, priv);

	conn = &priv->connector;
	preferred = find_preferred_mode(conn);
	KUNIT_ASSERT_NOT_NULL(test, preferred);

	KUNIT_EXPECT_EQ(test, preferred->hdisplay, 1920);
	KUNIT_EXPECT_EQ(test, preferred->vdisplay, 1080);
	KUNIT_EXPECT_EQ(test, preferred->clock, 148500);
}

/*
 * Test that the drm_hdmi_connector_mode_valid() will reject modes depending on
 * the .tmds_char_rate_valid() behaviour.
 * Pass a correct EDID and verify that high-rate modes are filtered.
 */
static void drm_test_check_mode_valid_reject_rate(struct kunit *test)
{
	struct drm_atomic_helper_connector_hdmi_priv *priv;
	struct drm_display_mode *preferred;

	priv = drm_kunit_helper_connector_hdmi_init_with_edid_funcs(test,
					&reject_100mhz_connector_hdmi_funcs,
					test_edid_hdmi_1080p_rgb_max_200mhz);
	KUNIT_ASSERT_NOT_NULL(test, priv);

	/*
	 * Unlike the drm_test_check_mode_valid() here 1080p is rejected, but
	 * 480p is allowed.
	 */
	preferred = find_preferred_mode(&priv->connector);
	KUNIT_ASSERT_NOT_NULL(test, preferred);
	KUNIT_EXPECT_EQ(test, preferred->hdisplay, 640);
	KUNIT_EXPECT_EQ(test, preferred->vdisplay, 480);
	KUNIT_EXPECT_EQ(test, preferred->clock, 25200);
}

/*
 * Test that the drm_hdmi_connector_mode_valid() will not mark any modes as
 * valid if .tmds_char_rate_valid() rejects all of them. Pass a correct EDID
 * and verify that there is no preferred mode and no modes were set for the
 * connector.
 */
static void drm_test_check_mode_valid_reject(struct kunit *test)
{
	struct drm_atomic_helper_connector_hdmi_priv *priv;
	struct drm_connector *conn;
	struct drm_display_mode *preferred;
	unsigned char no_edid[] = {};
	int ret;

	priv = drm_kunit_helper_connector_hdmi_init_with_edid_funcs(test,
					&reject_connector_hdmi_funcs,
					no_edid);
	KUNIT_ASSERT_NOT_NULL(test, priv);

	conn = &priv->connector;

	/* should reject all modes */
	ret = set_connector_edid(test, conn,
				 test_edid_hdmi_1080p_rgb_max_200mhz,
				 ARRAY_SIZE(test_edid_hdmi_1080p_rgb_max_200mhz));
	KUNIT_ASSERT_EQ(test, ret, 0);

	preferred = find_preferred_mode(conn);
	KUNIT_ASSERT_NULL(test, preferred);
}

/*
 * Test that the drm_hdmi_connector_mode_valid() will reject modes that don't
 * pass the info.max_tmds_clock filter. Pass crafted EDID and verify that
 * high-rate modes are filtered.
 */
static void drm_test_check_mode_valid_reject_max_clock(struct kunit *test)
{
	struct drm_atomic_helper_connector_hdmi_priv *priv;
	struct drm_connector *conn;
	struct drm_display_mode *preferred;

	priv = drm_kunit_helper_connector_hdmi_init_with_edid_funcs(test,
				&dummy_connector_hdmi_funcs,
				test_edid_hdmi_1080p_rgb_max_100mhz);
	KUNIT_ASSERT_NOT_NULL(test, priv);

	conn = &priv->connector;
	KUNIT_ASSERT_EQ(test, conn->display_info.max_tmds_clock, 100 * 1000);

	preferred = find_preferred_mode(conn);
	KUNIT_ASSERT_NOT_NULL(test, preferred);
	KUNIT_EXPECT_EQ(test, preferred->hdisplay, 640);
	KUNIT_EXPECT_EQ(test, preferred->vdisplay, 480);
	KUNIT_EXPECT_EQ(test, preferred->clock, 25200);
}

/*
 * Test that the max_tmds_char_rate inferred from caps.supported_hdmi_ver
 * is applied as a mode-filtering limit: with an HDMI 1.3 connector and no
 * driver hook, drm_hdmi_connector_mode_valid() will reject modes whose TMDS
 * character rate exceeds 340 MHz, even though the sink itself advertises
 * a higher limit (600 MHz).
 */
static void
drm_test_check_mode_valid_reject_inferred_max_tmds_char_rate(struct kunit *test)
{
	struct drm_connector_hdmi_funcs hdmi_funcs = dummy_connector_hdmi_funcs;
	struct drm_atomic_helper_connector_hdmi_priv *priv;
	struct drm_display_mode *preferred;

	hdmi_funcs.supported_hdmi_ver = HDMI_VERSION_1_3;

	priv = drm_kunit_helper_connector_hdmi_init_with_edid_funcs(test, &hdmi_funcs,
					test_edid_hdmi_4k_rgb_yuv420_dc_max_600mhz);
	KUNIT_ASSERT_NOT_NULL(test, priv);

	KUNIT_ASSERT_EQ(test, priv->connector.hdmi.max_tmds_char_rate,
			HDMI_1_3_TMDS_CHAR_RATE_MAX_HZ);

	preferred = find_preferred_mode(&priv->connector);
	KUNIT_ASSERT_NOT_NULL(test, preferred);
	KUNIT_EXPECT_EQ(test, preferred->hdisplay, 2560);
	KUNIT_EXPECT_EQ(test, preferred->vdisplay, 1600);
	KUNIT_EXPECT_EQ(test, preferred->clock, 268500);
}

/*
 * Test that a connector-provided max_tmds_char_rate limit (165 MHz) takes
 * precedence over a permissive .tmds_char_rate_valid() driver hook and
 * drm_hdmi_connector_mode_valid() will reject modes whose computed TMDS
 * character exceeds the connector limit.
 */
static void
drm_test_check_mode_valid_connector_max_tmds_char_rate_precedes_driver_hook(struct kunit *test)
{
	struct drm_atomic_helper_connector_hdmi_priv *priv;
	struct drm_display_mode *preferred;

	priv = drm_kunit_helper_connector_hdmi_init_with_edid_funcs(test,
					&reject_over_165mhz_connector_hdmi_funcs,
					test_edid_hdmi_4k_rgb_yuv420_dc_max_600mhz);
	KUNIT_ASSERT_NOT_NULL(test, priv);

	KUNIT_ASSERT_EQ(test, priv->connector.hdmi.max_tmds_char_rate,
			HDMI_1_0_TMDS_CHAR_RATE_MAX_HZ);

	preferred = find_preferred_mode(&priv->connector);
	KUNIT_ASSERT_NOT_NULL(test, preferred);
	KUNIT_EXPECT_EQ(test, preferred->hdisplay, 2048);
	KUNIT_EXPECT_EQ(test, preferred->vdisplay, 1152);
	KUNIT_EXPECT_EQ(test, preferred->clock, 162000);
}

/*
 * Test that a connector-provided max_tmds_char_rate (165 MHz) can be further
 * limited by a more restrictive (100 MHz) .tmds_char_rate_valid() driver hook
 * and drm_hdmi_connector_mode_valid() will reject modes whose computed TMDS
 * character exceeds the hook limit.
 */
static void
drm_test_check_mode_valid_driver_hook_limits_connector_max_tmds_char_rate(struct kunit *test)
{
	struct drm_connector_hdmi_funcs hdmi_funcs = reject_100mhz_connector_hdmi_funcs;
	struct drm_atomic_helper_connector_hdmi_priv *priv;
	struct drm_display_mode *preferred;

	hdmi_funcs.supported_hdmi_ver = HDMI_VERSION_1_3;
	hdmi_funcs.supported_tmds_char_rate = HDMI_1_0_TMDS_CHAR_RATE_MAX_HZ;

	priv = drm_kunit_helper_connector_hdmi_init_with_edid_funcs(test, &hdmi_funcs,
					test_edid_hdmi_4k_rgb_yuv420_dc_max_600mhz);
	KUNIT_ASSERT_NOT_NULL(test, priv);

	KUNIT_ASSERT_EQ(test, priv->connector.hdmi.max_tmds_char_rate,
			HDMI_1_0_TMDS_CHAR_RATE_MAX_HZ);

	preferred = find_preferred_mode(&priv->connector);
	KUNIT_ASSERT_NOT_NULL(test, preferred);
	KUNIT_EXPECT_EQ(test, preferred->hdisplay, 1440);
	KUNIT_EXPECT_EQ(test, preferred->vdisplay, 900);
	KUNIT_EXPECT_EQ(test, preferred->clock, 88750);
}

/*
 * Test that drm_hdmi_connector_mode_valid() will accept modes that require a
 * 4:2:0 chroma subsampling, even if said mode would violate maximum clock
 * constraints if it used RGB 4:4:4.
 */
static void drm_test_check_mode_valid_yuv420_only_max_clock(struct kunit *test)
{
	struct drm_connector_hdmi_funcs hdmi_funcs = dummy_connector_hdmi_funcs;
	struct drm_atomic_helper_connector_hdmi_priv *priv;
	struct drm_display_mode *dank;
	struct drm_connector *conn;

	hdmi_funcs.supported_formats = BIT(HDMI_COLORSPACE_RGB) |
				       BIT(HDMI_COLORSPACE_YUV420);
	priv = drm_kunit_helper_connector_hdmi_init_with_edid_funcs(test,
				&hdmi_funcs,
				test_edid_hdmi_1080p_rgb_yuv_4k_yuv420_dc_max_200mhz);
	KUNIT_ASSERT_NOT_NULL(test, priv);

	conn = &priv->connector;
	KUNIT_ASSERT_EQ(test, conn->display_info.max_tmds_clock, 200 * 1000);

	dank = find_420_only_mode(conn);
	KUNIT_ASSERT_NOT_NULL(test, dank);
	KUNIT_EXPECT_EQ(test, dank->hdisplay, 3840);
	KUNIT_EXPECT_EQ(test, dank->vdisplay, 2160);

	/*
	 * Note: The mode's "clock" here is not accurate to the actual TMDS
	 * clock that HDMI will use for a subsampled mode. Hence, why the mode's
	 * clock is above the .max_tmds_clock of 200MHz.
	 */
	KUNIT_EXPECT_EQ(test, dank->clock, 297000);
}

/*
 * Test that drm_hdmi_connector_mode_valid() will reject modes that require
 * 4:2:0 chroma subsampling, if the connector does not support 4:2:0.
 */
static void
drm_test_check_mode_valid_reject_yuv420_only_connector(struct kunit *test)
{
	struct drm_atomic_helper_connector_hdmi_priv *priv;
	struct drm_display_mode *dank;
	struct drm_connector *conn;

	priv = drm_kunit_helper_connector_hdmi_init_with_edid_funcs(test,
				&dummy_connector_hdmi_funcs,
				test_edid_hdmi_1080p_rgb_yuv_4k_yuv420_dc_max_200mhz);
	KUNIT_ASSERT_NOT_NULL(test, priv);

	conn = &priv->connector;
	KUNIT_ASSERT_EQ(test, conn->display_info.max_tmds_clock, 200 * 1000);

	dank = find_420_only_mode(conn);
	KUNIT_EXPECT_NULL(test, dank);
}

/*
 * Test that drm_hdmi_connector_mode_valid() will accept modes that allow (among
 * other color formats) 4:2:0 chroma subsampling, even if the connector does not
 * support 4:2:0, but the mode's clock works for RGB 4:4:4.
 */
static void
drm_test_check_mode_valid_accept_yuv420_also_connector_rgb(struct kunit *test)
{
	struct drm_atomic_helper_connector_hdmi_priv *priv;
	struct drm_display_mode *mode;
	struct drm_connector *conn;

	priv = drm_kunit_helper_connector_hdmi_init_with_edid_funcs(test,
				&dummy_connector_hdmi_funcs,
				test_edid_hdmi_4k_rgb_yuv420_dc_max_340mhz);
	KUNIT_ASSERT_NOT_NULL(test, priv);

	conn = &priv->connector;
	KUNIT_ASSERT_EQ(test, conn->display_info.max_tmds_clock, 340 * 1000);

	mode = find_420_also_mode(conn);
	KUNIT_ASSERT_NOT_NULL(test, mode);
	KUNIT_EXPECT_EQ(test, mode->hdisplay, 3840);
	KUNIT_EXPECT_EQ(test, mode->vdisplay, 2160);
	KUNIT_EXPECT_EQ(test, mode->clock, 297000);
}

static struct kunit_case drm_atomic_helper_connector_hdmi_mode_valid_tests[] = {
	KUNIT_CASE(drm_test_check_mode_valid),
	KUNIT_CASE(drm_test_check_mode_valid_reject),
	KUNIT_CASE(drm_test_check_mode_valid_reject_rate),
	KUNIT_CASE(drm_test_check_mode_valid_reject_max_clock),
	KUNIT_CASE(drm_test_check_mode_valid_reject_inferred_max_tmds_char_rate),
	KUNIT_CASE(drm_test_check_mode_valid_connector_max_tmds_char_rate_precedes_driver_hook),
	KUNIT_CASE(drm_test_check_mode_valid_driver_hook_limits_connector_max_tmds_char_rate),
	KUNIT_CASE(drm_test_check_mode_valid_yuv420_only_max_clock),
	KUNIT_CASE(drm_test_check_mode_valid_reject_yuv420_only_connector),
	KUNIT_CASE(drm_test_check_mode_valid_accept_yuv420_also_connector_rgb),
	{ }
};

static struct kunit_suite drm_atomic_helper_connector_hdmi_mode_valid_test_suite = {
	.name		= "drm_atomic_helper_connector_hdmi_mode_valid",
	.test_cases	= drm_atomic_helper_connector_hdmi_mode_valid_tests,
};

/*
 * Test that the default behaviour works without errors. We expect that
 * infoframe-related hooks are called and there are no errors raised.
 */
static void drm_test_check_infoframes(struct kunit *test)
{
	struct drm_atomic_helper_connector_hdmi_priv *priv;
	struct drm_modeset_acquire_ctx ctx;
	struct drm_crtc_state *crtc_state;
	struct drm_atomic_commit *state;
	struct drm_display_mode *preferred;
	struct drm_connector *conn;
	struct drm_device *drm;
	struct drm_crtc *crtc;
	int old_hdmi_update_failures;
	int ret;

	priv = drm_kunit_helper_connector_hdmi_init_with_edid_funcs(test,
				&dummy_connector_hdmi_funcs,
				test_edid_hdmi_1080p_rgb_max_200mhz);
	KUNIT_ASSERT_NOT_NULL(test, priv);

	drm = &priv->drm;
	crtc = priv->crtc;
	conn = &priv->connector;

	preferred = find_preferred_mode(conn);
	KUNIT_ASSERT_NOT_NULL(test, preferred);

	drm_modeset_acquire_init(&ctx, 0);

retry_conn_enable:
	ret = drm_kunit_helper_enable_crtc_connector(test, drm,
						     crtc, conn,
						     preferred,
						     &ctx);
	if (ret == -EDEADLK) {
		ret = drm_modeset_backoff(&ctx);
		if (!ret)
			goto retry_conn_enable;
	}
	KUNIT_ASSERT_EQ(test, ret, 0);

	state = drm_kunit_helper_atomic_state_alloc(test, drm, &ctx);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, state);

retry_crtc_state:
	crtc_state = drm_atomic_get_crtc_state(state, crtc);
	if (PTR_ERR(crtc_state) == -EDEADLK) {
		drm_atomic_commit_clear(state);
		ret = drm_modeset_backoff(&ctx);
		if (!ret)
			goto retry_crtc_state;
	}
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, crtc_state);

	crtc_state->mode_changed = true;

	old_hdmi_update_failures = priv->hdmi_update_failures;

	ret = drm_atomic_check_only(state);
	if (ret == -EDEADLK) {
		drm_atomic_commit_clear(state);
		ret = drm_modeset_backoff(&ctx);
		if (!ret)
			goto retry_crtc_state;
	}
	KUNIT_ASSERT_EQ(test, ret, 0);

	ret = drm_atomic_commit(state);
	if (ret == -EDEADLK) {
		drm_atomic_commit_clear(state);
		ret = drm_modeset_backoff(&ctx);
		if (!ret)
			goto retry_crtc_state;
	}
	KUNIT_ASSERT_EQ(test, ret, 0);

	KUNIT_EXPECT_GE(test, old_hdmi_update_failures, priv->hdmi_update_failures);

	drm_modeset_drop_locks(&ctx);
	drm_modeset_acquire_fini(&ctx);
}

struct drm_hdmi_spd_infoframe_test {
	const char *name;
	const char *vendor;
	const char *product;
	unsigned char expected_vendor[HDMI_SPD_INFOFRAME_VENDOR_LEN];
	unsigned char expected_product[HDMI_SPD_INFOFRAME_PRODUCT_LEN];
};

static const struct drm_hdmi_spd_infoframe_test drm_hdmi_spd_infoframe_tests[] = {
	{
		.name = "shorter-than-field",
		.vendor = "Vend",
		.product = "Prod",
		.expected_vendor = { 'V', 'e', 'n', 'd' },
		.expected_product = { 'P', 'r', 'o', 'd' },
	},
	{
		.name = "whole-field",
		.vendor = "VendorVe",
		.product = "ProductProductPr",
		.expected_vendor = {
			'V', 'e', 'n', 'd', 'o', 'r', 'V', 'e',
		},
		.expected_product = {
			'P', 'r', 'o', 'd', 'u', 'c', 't',
			'P', 'r', 'o', 'd', 'u', 'c', 't',
			'P', 'r',
		},
	},
};

static void drm_hdmi_spd_infoframe_desc(const struct drm_hdmi_spd_infoframe_test *t,
					char *desc)
{
	strscpy(desc, t->name, KUNIT_PARAM_DESC_SIZE);
}

KUNIT_ARRAY_PARAM(drm_hdmi_spd_infoframe, drm_hdmi_spd_infoframe_tests,
		  drm_hdmi_spd_infoframe_desc);

/*
 * The SPD InfoFrame is only generated for connectors implementing the
 * related hooks, hence the SPD tests cannot rely on the plain dummy funcs.
 */
static const struct drm_connector_hdmi_funcs dummy_connector_hdmi_spd_funcs = {
	.vendor = "Vendor",
	.product = "Product",
	.supported_hdmi_ver = HDMI_VERSION_1_4,
	.supported_formats = BIT(DRM_OUTPUT_COLOR_FORMAT_RGB444),
	.max_bpc = 8,
	.avi = {
		.clear_infoframe = accept_infoframe_clear_infoframe,
		.write_infoframe = accept_infoframe_write_infoframe,
	},
	.spd = {
		.clear_infoframe = accept_infoframe_clear_infoframe,
		.write_infoframe = accept_infoframe_write_infoframe,
	},
	.hdmi = {
		.clear_infoframe = accept_infoframe_clear_infoframe,
		.write_infoframe = accept_infoframe_write_infoframe,
	},
};

/*
 * Test that the vendor and product names end up in the SPD InfoFrame padded
 * with zeros, and without any trailing NUL when spanning the whole field.
 */
static void drm_test_check_spd_infoframe(struct kunit *test)
{
	const struct drm_hdmi_spd_infoframe_test *param = test->param_value;
	struct drm_connector_hdmi_funcs hdmi_funcs = dummy_connector_hdmi_spd_funcs;
	struct drm_atomic_helper_connector_hdmi_priv *priv;
	struct drm_modeset_acquire_ctx ctx;
	struct drm_connector_state *conn_state;
	struct drm_crtc_state *crtc_state;
	struct drm_atomic_commit *state;
	struct drm_display_mode *preferred;
	struct hdmi_spd_infoframe *frame;
	struct drm_connector *conn;
	struct drm_device *drm;
	struct drm_crtc *crtc;
	int ret;

	hdmi_funcs.vendor = param->vendor;
	hdmi_funcs.product = param->product;

	priv = drm_kunit_helper_connector_hdmi_init_with_edid_funcs(test,
				&hdmi_funcs,
				test_edid_hdmi_1080p_rgb_max_200mhz);
	KUNIT_ASSERT_NOT_NULL(test, priv);

	drm = &priv->drm;
	crtc = priv->crtc;
	conn = &priv->connector;

	preferred = find_preferred_mode(conn);
	KUNIT_ASSERT_NOT_NULL(test, preferred);

	drm_modeset_acquire_init(&ctx, 0);

retry_conn_enable:
	ret = drm_kunit_helper_enable_crtc_connector(test, drm,
						     crtc, conn,
						     preferred,
						     &ctx);
	if (ret == -EDEADLK) {
		ret = drm_modeset_backoff(&ctx);
		if (!ret)
			goto retry_conn_enable;
	}
	KUNIT_ASSERT_EQ(test, ret, 0);

	state = drm_kunit_helper_atomic_state_alloc(test, drm, &ctx);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, state);

retry_crtc_state:
	crtc_state = drm_atomic_get_crtc_state(state, crtc);
	if (PTR_ERR(crtc_state) == -EDEADLK) {
		drm_atomic_commit_clear(state);
		ret = drm_modeset_backoff(&ctx);
		if (!ret)
			goto retry_crtc_state;
	}
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, crtc_state);

	crtc_state->mode_changed = true;

	ret = drm_atomic_check_only(state);
	if (ret == -EDEADLK) {
		drm_atomic_commit_clear(state);
		ret = drm_modeset_backoff(&ctx);
		if (!ret)
			goto retry_crtc_state;
	}
	KUNIT_ASSERT_EQ(test, ret, 0);

	ret = drm_atomic_commit(state);
	if (ret == -EDEADLK) {
		drm_atomic_commit_clear(state);
		ret = drm_modeset_backoff(&ctx);
		if (!ret)
			goto retry_crtc_state;
	}
	KUNIT_ASSERT_EQ(test, ret, 0);

	conn_state = conn->state;
	KUNIT_ASSERT_NOT_NULL(test, conn_state);
	KUNIT_ASSERT_TRUE(test, conn_state->hdmi.infoframes.spd.set);

	frame = &conn_state->hdmi.infoframes.spd.data.spd;

	KUNIT_EXPECT_MEMEQ(test, frame->vendor, param->expected_vendor,
			   sizeof(frame->vendor));
	KUNIT_EXPECT_MEMEQ(test, frame->product, param->expected_product,
			   sizeof(frame->product));

	drm_modeset_drop_locks(&ctx);
	drm_modeset_acquire_fini(&ctx);
}

static int reject_infoframe_write_infoframe(struct drm_connector *connector,
					    const u8 *buffer, size_t len)
{
	return -EOPNOTSUPP;
}

static const struct drm_connector_hdmi_funcs reject_avi_infoframe_hdmi_funcs = {
	.vendor = "Vendor",
	.product = "Product",
	.supported_hdmi_ver = HDMI_VERSION_1_4,
	.supported_formats = BIT(DRM_OUTPUT_COLOR_FORMAT_RGB444),
	.max_bpc = 8,
	.avi = {
		.clear_infoframe = accept_infoframe_clear_infoframe,
		.write_infoframe = reject_infoframe_write_infoframe,
	},
	.hdmi = {
		.clear_infoframe = accept_infoframe_clear_infoframe,
		.write_infoframe = accept_infoframe_write_infoframe,
	},
};

/*
 * Test that the rejection of AVI InfoFrame results in the failure of
 * drm_atomic_helper_connector_hdmi_update_infoframes().
 */
static void drm_test_check_reject_avi_infoframe(struct kunit *test)
{
	struct drm_atomic_helper_connector_hdmi_priv *priv;
	struct drm_modeset_acquire_ctx ctx;
	struct drm_atomic_commit *state;
	struct drm_crtc_state *crtc_state;
	struct drm_display_mode *preferred;
	struct drm_connector *conn;
	struct drm_device *drm;
	struct drm_crtc *crtc;
	int old_hdmi_update_failures;
	int ret;

	priv = drm_kunit_helper_connector_hdmi_init_with_edid_funcs(test,
				&reject_avi_infoframe_hdmi_funcs,
				test_edid_hdmi_1080p_rgb_max_200mhz);
	KUNIT_ASSERT_NOT_NULL(test, priv);

	drm = &priv->drm;
	crtc = priv->crtc;
	conn = &priv->connector;

	preferred = find_preferred_mode(conn);
	KUNIT_ASSERT_NOT_NULL(test, preferred);

	drm_modeset_acquire_init(&ctx, 0);

retry_conn_enable:
	ret = drm_kunit_helper_enable_crtc_connector(test, drm,
						     crtc, conn,
						     preferred,
						     &ctx);
	if (ret == -EDEADLK) {
		ret = drm_modeset_backoff(&ctx);
		if (!ret)
			goto retry_conn_enable;
	}
	KUNIT_ASSERT_EQ(test, ret, 0);

	drm_encoder_helper_add(&priv->encoder, &test_encoder_helper_funcs);

	state = drm_kunit_helper_atomic_state_alloc(test, drm, &ctx);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, state);

retry_crtc_state:
	crtc_state = drm_atomic_get_crtc_state(state, crtc);
	if (PTR_ERR(crtc_state) == -EDEADLK) {
		drm_atomic_commit_clear(state);
		ret = drm_modeset_backoff(&ctx);
		if (!ret)
			goto retry_crtc_state;
	}
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, crtc_state);

	crtc_state->mode_changed = true;

	old_hdmi_update_failures = priv->hdmi_update_failures;

	ret = drm_atomic_check_only(state);
	if (ret == -EDEADLK) {
		drm_atomic_commit_clear(state);
		ret = drm_modeset_backoff(&ctx);
		if (!ret)
			goto retry_crtc_state;
	}
	KUNIT_ASSERT_EQ(test, ret, 0);

	ret = drm_atomic_commit(state);
	if (ret == -EDEADLK) {
		drm_atomic_commit_clear(state);
		ret = drm_modeset_backoff(&ctx);
		if (!ret)
			goto retry_crtc_state;
	}
	KUNIT_ASSERT_EQ(test, ret, 0);

	KUNIT_EXPECT_NE(test, old_hdmi_update_failures, priv->hdmi_update_failures);

	drm_modeset_drop_locks(&ctx);
	drm_modeset_acquire_fini(&ctx);
}

static const struct drm_connector_hdmi_funcs reject_hdr_infoframe_hdmi_funcs = {
	.vendor = "Vendor",
	.product = "Product",
	.supported_hdmi_ver = HDMI_VERSION_1_4,
	.supported_formats = BIT(DRM_OUTPUT_COLOR_FORMAT_RGB444),
	.max_bpc = 8,
	.avi = {
		.clear_infoframe = accept_infoframe_clear_infoframe,
		.write_infoframe = accept_infoframe_write_infoframe,
	},
	.hdmi = {
		.clear_infoframe = accept_infoframe_clear_infoframe,
		.write_infoframe = accept_infoframe_write_infoframe,
	},
	.hdr_drm = {
		.clear_infoframe = accept_infoframe_clear_infoframe,
		.write_infoframe = reject_infoframe_write_infoframe,
	},
};

/*
 * Test that the HDR InfoFrame isn't programmed in
 * drm_atomic_helper_connector_hdmi_update_infoframes() if the max_bpc is 8.
 */
static void drm_test_check_reject_hdr_infoframe_bpc_8(struct kunit *test)
{
	struct drm_atomic_helper_connector_hdmi_priv *priv;
	struct drm_modeset_acquire_ctx ctx;
	struct drm_atomic_commit *state;
	struct drm_connector_state *new_conn_state;
	struct drm_crtc_state *crtc_state;
	struct drm_display_mode *preferred;
	struct drm_connector *conn;
	struct drm_device *drm;
	struct drm_crtc *crtc;
	int old_hdmi_update_failures;
	int ret;

	priv = drm_kunit_helper_connector_hdmi_init_with_edid_funcs(test,
				&reject_hdr_infoframe_hdmi_funcs,
				test_edid_hdmi_1080p_rgb_max_200mhz_hdr);
	KUNIT_ASSERT_NOT_NULL(test, priv);

	drm = &priv->drm;
	crtc = priv->crtc;
	conn = &priv->connector;

	preferred = find_preferred_mode(conn);
	KUNIT_ASSERT_NOT_NULL(test, preferred);

	drm_modeset_acquire_init(&ctx, 0);

retry_conn_enable:
	ret = drm_kunit_helper_enable_crtc_connector(test, drm,
						     crtc, conn,
						     preferred,
						     &ctx);
	if (ret == -EDEADLK) {
		ret = drm_modeset_backoff(&ctx);
		if (!ret)
			goto retry_conn_enable;
	}
	KUNIT_ASSERT_EQ(test, ret, 0);

	drm_encoder_helper_add(&priv->encoder, &test_encoder_helper_funcs);

	state = drm_kunit_helper_atomic_state_alloc(test, drm, &ctx);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, state);

retry_conn_state:
	new_conn_state = drm_atomic_get_connector_state(state, conn);
	if (PTR_ERR(new_conn_state) == -EDEADLK) {
		drm_atomic_commit_clear(state);
		ret = drm_modeset_backoff(&ctx);
		if (!ret)
			goto retry_conn_state;
	}
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, new_conn_state);

	crtc_state = drm_atomic_get_crtc_state(state, crtc);
	if (PTR_ERR(crtc_state) == -EDEADLK) {
		drm_atomic_commit_clear(state);
		ret = drm_modeset_backoff(&ctx);
		if (!ret)
			goto retry_conn_state;
	}
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, crtc_state);

	/* Verify that there is no HDR property, so "userspace" can't set it. */
	for (int i = 0; i < conn->base.properties->count; i++)
		KUNIT_ASSERT_PTR_NE(test,
				    drm->mode_config.hdr_output_metadata_property,
				    conn->base.properties->properties[i]);

	crtc_state->mode_changed = true;

	old_hdmi_update_failures = priv->hdmi_update_failures;

	ret = drm_atomic_check_only(state);
	if (ret == -EDEADLK) {
		drm_atomic_commit_clear(state);
		ret = drm_modeset_backoff(&ctx);
		if (!ret)
			goto retry_conn_state;
	}
	KUNIT_ASSERT_EQ(test, ret, 0);

	ret = drm_atomic_commit(state);
	if (ret == -EDEADLK) {
		drm_atomic_commit_clear(state);
		ret = drm_modeset_backoff(&ctx);
		if (!ret)
			goto retry_conn_state;
	}
	KUNIT_ASSERT_EQ(test, ret, 0);

	KUNIT_EXPECT_EQ(test, old_hdmi_update_failures, priv->hdmi_update_failures);

	new_conn_state = conn->state;
	KUNIT_ASSERT_NOT_NULL(test, new_conn_state);

	KUNIT_ASSERT_EQ(test, new_conn_state->hdmi.output_bpc, 8);
	KUNIT_ASSERT_EQ(test, new_conn_state->hdmi.infoframes.hdr_drm.set, false);

	drm_modeset_drop_locks(&ctx);
	drm_modeset_acquire_fini(&ctx);
}

/*
 * Test that the rejection of HDR InfoFrame results in the failure of
 * drm_atomic_helper_connector_hdmi_update_infoframes() in the high bpc is
 * supported.
 */
static void drm_test_check_reject_hdr_infoframe_bpc_10(struct kunit *test)
{
	struct drm_connector_hdmi_funcs hdmi_funcs = reject_hdr_infoframe_hdmi_funcs;
	struct drm_atomic_helper_connector_hdmi_priv *priv;
	struct drm_modeset_acquire_ctx ctx;
	struct drm_atomic_commit *state;
	struct drm_connector_state *new_conn_state;
	struct drm_crtc_state *crtc_state;
	struct drm_display_mode *preferred;
	struct drm_connector *conn;
	struct drm_device *drm;
	struct drm_crtc *crtc;
	int old_hdmi_update_failures;
	struct hdr_output_metadata hdr_data;
	struct drm_property_blob *hdr_blob;
	bool replaced;
	int ret;

	hdmi_funcs.max_bpc = 10;
	priv = drm_kunit_helper_connector_hdmi_init_with_edid_funcs(test,
				&hdmi_funcs,
				test_edid_hdmi_1080p_rgb_max_200mhz_hdr);
	KUNIT_ASSERT_NOT_NULL(test, priv);

	drm = &priv->drm;
	crtc = priv->crtc;
	conn = &priv->connector;

	preferred = find_preferred_mode(conn);
	KUNIT_ASSERT_NOT_NULL(test, preferred);

	drm_modeset_acquire_init(&ctx, 0);

retry_conn_enable:
	ret = drm_kunit_helper_enable_crtc_connector(test, drm,
						     crtc, conn,
						     preferred,
						     &ctx);
	if (ret == -EDEADLK) {
		ret = drm_modeset_backoff(&ctx);
		if (!ret)
			goto retry_conn_enable;
	}
	KUNIT_ASSERT_EQ(test, ret, 0);

	drm_encoder_helper_add(&priv->encoder, &test_encoder_helper_funcs);

	state = drm_kunit_helper_atomic_state_alloc(test, drm, &ctx);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, state);

retry_conn_state:
	new_conn_state = drm_atomic_get_connector_state(state, conn);
	if (PTR_ERR(new_conn_state) == -EDEADLK) {
		drm_atomic_commit_clear(state);
		ret = drm_modeset_backoff(&ctx);
		if (!ret)
			goto retry_conn_state;
	}
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, new_conn_state);

	crtc_state = drm_atomic_get_crtc_state(state, crtc);
	if (PTR_ERR(crtc_state) == -EDEADLK) {
		drm_atomic_commit_clear(state);
		ret = drm_modeset_backoff(&ctx);
		if (!ret)
			goto retry_conn_state;
	}
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, crtc_state);

	hdr_data.metadata_type = HDMI_STATIC_METADATA_TYPE1;
	hdr_data.hdmi_metadata_type1.eotf = HDMI_EOTF_TRADITIONAL_GAMMA_SDR;
	hdr_data.hdmi_metadata_type1.metadata_type = HDMI_STATIC_METADATA_TYPE1;

	hdr_blob = drm_property_create_blob(drm, sizeof(hdr_data), &hdr_data);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, hdr_blob);

	ret = drm_property_replace_blob_from_id(drm,
						&new_conn_state->hdr_output_metadata,
						hdr_blob->base.id,
						-1, sizeof(struct hdr_output_metadata), -1,
						&replaced);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_ASSERT_EQ(test, replaced, true);

	crtc_state->mode_changed = true;

	old_hdmi_update_failures = priv->hdmi_update_failures;

	ret = drm_atomic_check_only(state);
	if (ret == -EDEADLK) {
		drm_atomic_commit_clear(state);
		ret = drm_modeset_backoff(&ctx);
		if (!ret)
			goto retry_conn_state;
	}
	KUNIT_ASSERT_EQ(test, ret, 0);

	ret = drm_atomic_commit(state);
	if (ret == -EDEADLK) {
		drm_atomic_commit_clear(state);
		ret = drm_modeset_backoff(&ctx);
		if (!ret)
			goto retry_conn_state;
	}
	KUNIT_ASSERT_EQ(test, ret, 0);

	KUNIT_EXPECT_LE(test, old_hdmi_update_failures, priv->hdmi_update_failures);

	new_conn_state = conn->state;
	KUNIT_ASSERT_NOT_NULL(test, new_conn_state);

	KUNIT_ASSERT_EQ(test, new_conn_state->hdmi.output_bpc, 10);
	KUNIT_ASSERT_EQ(test, new_conn_state->hdmi.infoframes.hdr_drm.set, true);

	drm_modeset_drop_locks(&ctx);
	drm_modeset_acquire_fini(&ctx);
}

static const struct drm_connector_hdmi_funcs reject_audio_infoframe_hdmi_funcs = {
	.vendor = "Vendor",
	.product = "Product",
	.supported_hdmi_ver = HDMI_VERSION_1_4,
	.supported_formats = BIT(DRM_OUTPUT_COLOR_FORMAT_RGB444),
	.max_bpc = 8,
	.avi = {
		.clear_infoframe = accept_infoframe_clear_infoframe,
		.write_infoframe = accept_infoframe_write_infoframe,
	},
	.hdmi = {
		.clear_infoframe = accept_infoframe_clear_infoframe,
		.write_infoframe = accept_infoframe_write_infoframe,
	},
	.audio = {
		.clear_infoframe = accept_infoframe_clear_infoframe,
		.write_infoframe = reject_infoframe_write_infoframe,
	},
};

/*
 * Test that Audio InfoFrame is only programmed if we call a corresponding API,
 * thus the drivers can safely assume that they won't get Audio InfoFrames if
 * they don't call it.
 */
static void drm_test_check_reject_audio_infoframe(struct kunit *test)
{
	struct drm_atomic_helper_connector_hdmi_priv *priv;
	struct drm_modeset_acquire_ctx ctx;
	struct drm_atomic_commit *state;
	struct drm_crtc_state *crtc_state;
	struct drm_display_mode *preferred;
	struct drm_connector *conn;
	struct drm_device *drm;
	struct drm_crtc *crtc;
	int old_hdmi_update_failures;
	struct hdmi_audio_infoframe cea;
	int ret;

	priv = drm_kunit_helper_connector_hdmi_init_with_edid_funcs(test,
				&reject_audio_infoframe_hdmi_funcs,
				test_edid_hdmi_1080p_rgb_max_200mhz);
	KUNIT_ASSERT_NOT_NULL(test, priv);

	drm = &priv->drm;
	crtc = priv->crtc;
	conn = &priv->connector;

	preferred = find_preferred_mode(conn);
	KUNIT_ASSERT_NOT_NULL(test, preferred);

	drm_modeset_acquire_init(&ctx, 0);

retry_conn_enable:
	ret = drm_kunit_helper_enable_crtc_connector(test, drm,
						     crtc, conn,
						     preferred,
						     &ctx);
	if (ret == -EDEADLK) {
		ret = drm_modeset_backoff(&ctx);
		if (!ret)
			goto retry_conn_enable;
	}
	KUNIT_ASSERT_EQ(test, ret, 0);

	drm_encoder_helper_add(&priv->encoder, &test_encoder_helper_funcs);

	state = drm_kunit_helper_atomic_state_alloc(test, drm, &ctx);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, state);

retry_crtc_state:
	crtc_state = drm_atomic_get_crtc_state(state, crtc);
	if (PTR_ERR(crtc_state) == -EDEADLK) {
		drm_atomic_commit_clear(state);
		ret = drm_modeset_backoff(&ctx);
		if (!ret)
			goto retry_crtc_state;
	}
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, crtc_state);

	crtc_state->mode_changed = true;

	old_hdmi_update_failures = priv->hdmi_update_failures;

	ret = drm_atomic_check_only(state);
	if (ret == -EDEADLK) {
		drm_atomic_commit_clear(state);
		ret = drm_modeset_backoff(&ctx);
		if (!ret)
			goto retry_crtc_state;
	}
	KUNIT_ASSERT_EQ(test, ret, 0);

	ret = drm_atomic_commit(state);
	if (ret == -EDEADLK) {
		drm_atomic_commit_clear(state);
		ret = drm_modeset_backoff(&ctx);
		if (!ret)
			goto retry_crtc_state;
	}
	KUNIT_ASSERT_EQ(test, ret, 0);

	KUNIT_EXPECT_EQ(test, old_hdmi_update_failures, priv->hdmi_update_failures);

	/*
	 * So, it works without Audio InfoFrame, let's fail with it in place,
	 * checking that writing the infofraem actually gets triggered.
	 */

	hdmi_audio_infoframe_init(&cea);
	cea.channels = 2;
	cea.coding_type = HDMI_AUDIO_CODING_TYPE_STREAM;
	cea.sample_size = HDMI_AUDIO_SAMPLE_SIZE_STREAM;
	cea.sample_frequency = HDMI_AUDIO_SAMPLE_FREQUENCY_STREAM;

	ret = drm_atomic_helper_connector_hdmi_update_audio_infoframe(conn, &cea);
	KUNIT_ASSERT_EQ(test, ret, -EOPNOTSUPP);

	state = drm_kunit_helper_atomic_state_alloc(test, drm, &ctx);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, state);

retry_crtc_state_2:
	crtc_state = drm_atomic_get_crtc_state(state, crtc);
	if (PTR_ERR(crtc_state) == -EDEADLK) {
		drm_atomic_commit_clear(state);
		ret = drm_modeset_backoff(&ctx);
		if (!ret)
			goto retry_crtc_state_2;
	}
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, crtc_state);

	crtc_state->mode_changed = true;

	old_hdmi_update_failures = priv->hdmi_update_failures;

	ret = drm_atomic_check_only(state);
	if (ret == -EDEADLK) {
		drm_atomic_commit_clear(state);
		ret = drm_modeset_backoff(&ctx);
		if (!ret)
			goto retry_crtc_state_2;
	}
	KUNIT_ASSERT_EQ(test, ret, 0);

	ret = drm_atomic_commit(state);
	if (ret == -EDEADLK) {
		drm_atomic_commit_clear(state);
		ret = drm_modeset_backoff(&ctx);
		if (!ret)
			goto retry_crtc_state_2;
	}
	KUNIT_ASSERT_EQ(test, ret, 0);

	KUNIT_EXPECT_LE(test, old_hdmi_update_failures, priv->hdmi_update_failures);

	drm_modeset_drop_locks(&ctx);
	drm_modeset_acquire_fini(&ctx);
}

static struct kunit_case drm_atomic_helper_connector_hdmi_infoframes_tests[] = {
	KUNIT_CASE(drm_test_check_infoframes),
	KUNIT_CASE_PARAM(drm_test_check_spd_infoframe,
			 drm_hdmi_spd_infoframe_gen_params),
	KUNIT_CASE(drm_test_check_reject_avi_infoframe),
	KUNIT_CASE(drm_test_check_reject_hdr_infoframe_bpc_8),
	KUNIT_CASE(drm_test_check_reject_hdr_infoframe_bpc_10),
	KUNIT_CASE(drm_test_check_reject_audio_infoframe),
	{ }
};

static struct kunit_suite drm_atomic_helper_connector_hdmi_infoframes_test_suite = {
	.name		= "drm_atomic_helper_connector_hdmi_infoframes",
	.test_cases	= drm_atomic_helper_connector_hdmi_infoframes_tests,
};

kunit_test_suites(
	&drm_atomic_helper_connector_hdmi_check_test_suite,
	&drm_atomic_helper_connector_hdmi_reset_test_suite,
	&drm_atomic_helper_connector_hdmi_mode_valid_test_suite,
	&drm_atomic_helper_connector_hdmi_infoframes_test_suite,
);

MODULE_AUTHOR("Maxime Ripard <mripard@kernel.org>");
MODULE_DESCRIPTION("Kunit test for drm_hdmi_state_helper functions");
MODULE_LICENSE("GPL");
