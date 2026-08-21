// SPDX-License-Identifier: GPL-2.0
/*
 * Kunit test for drm_modes functions
 */

#include <linux/i2c.h>

#include <drm/drm_atomic_state_helper.h>
#include <drm/drm_connector.h>
#include <drm/drm_drv.h>
#include <drm/drm_edid.h>
#include <drm/drm_file.h>
#include <drm/drm_kunit_helpers.h>
#include <drm/drm_modes.h>

#include <drm/display/drm_hdmi_helper.h>

#include <kunit/test.h>

#include "../drm_crtc_internal.h"

struct drm_connector_init_priv {
	struct drm_device drm;
	struct drm_connector connector;
	struct i2c_adapter ddc;
	/* Mutable copy of a static instance for test-specific overrides */
	struct drm_connector_hdmi_funcs hdmi_funcs;
};

static int accept_infoframe_clear_infoframe(struct drm_connector *connector)
{
	return 0;
}

static int accept_infoframe_write_infoframe(struct drm_connector *connector,
					    const u8 *buffer, size_t len)
{
	return 0;
}

static const struct drm_connector_hdmi_funcs dummy_hdmi_funcs = {
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

static int accept_scrambler_enable(struct drm_connector *connector)
{
	return 0;
}

static int accept_scrambler_disable(struct drm_connector *connector)
{
	return 0;
}

static const struct drm_connector_hdmi_funcs dummy_hdmi_funcs_scrambler = {
	.vendor = "Vendor",
	.product = "Product",
	.supported_hdmi_ver = HDMI_VERSION_2_0,
	.supported_formats = BIT(DRM_OUTPUT_COLOR_FORMAT_RGB444),
	.max_bpc = 8,
	.scrambler_enable = accept_scrambler_enable,
	.scrambler_disable = accept_scrambler_disable,
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

static int accept_frl_set_ffe_level(struct drm_connector *connector, u8 ffe_level)
{
	return 0;
}

static const struct drm_connector_hdmi_funcs dummy_hdmi_funcs_frl = {
	.vendor = "Vendor",
	.product = "Product",
	.supported_hdmi_ver = HDMI_VERSION_2_1,
	.supported_formats = BIT(DRM_OUTPUT_COLOR_FORMAT_RGB444),
	.max_bpc = 8,
	.scrambler_enable = accept_scrambler_enable,
	.scrambler_disable = accept_scrambler_disable,
	.frl_configure = accept_frl_configure,
	.frl_set_ltp = accept_frl_set_ltp,
	.frl_tx_start = accept_frl_tx_start,
	.frl_tx_stop = accept_frl_tx_stop,
	.frl_fallback_tmds = accept_frl_fallback_tmds,
	.avi = {
		.clear_infoframe = accept_infoframe_clear_infoframe,
		.write_infoframe = accept_infoframe_write_infoframe,
	},
	.hdmi = {
		.clear_infoframe = accept_infoframe_clear_infoframe,
		.write_infoframe = accept_infoframe_write_infoframe,
	},
};

static const struct drm_connector_funcs dummy_funcs = {
	.atomic_destroy_state	= drm_atomic_helper_connector_destroy_state,
	.atomic_duplicate_state	= drm_atomic_helper_connector_duplicate_state,
	.reset			= drm_atomic_helper_connector_reset,
};

static int dummy_ddc_xfer(struct i2c_adapter *adapter,
			  struct i2c_msg *msgs, int num)
{
	return num;
}

static u32 dummy_ddc_func(struct i2c_adapter *adapter)
{
	return I2C_FUNC_I2C | I2C_FUNC_SMBUS_EMUL;
}

static const struct i2c_algorithm dummy_ddc_algorithm = {
	.master_xfer = dummy_ddc_xfer,
	.functionality = dummy_ddc_func,
};

static void i2c_del_adapter_wrapper(void *ptr)
{
	struct i2c_adapter *adap = ptr;

	i2c_del_adapter(adap);
}

static int drm_test_connector_init(struct kunit *test)
{
	struct drm_connector_init_priv *priv;
	struct device *dev;
	int ret;

	dev = drm_kunit_helper_alloc_device(test);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, dev);

	priv = drm_kunit_helper_alloc_drm_device(test, dev,
						 struct drm_connector_init_priv, drm,
						 DRIVER_MODESET | DRIVER_ATOMIC);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, priv);

	strscpy(priv->ddc.name, "dummy-connector-ddc", sizeof(priv->ddc.name));
	priv->ddc.owner = THIS_MODULE;
	priv->ddc.algo = &dummy_ddc_algorithm;
	priv->ddc.dev.parent = dev;

	ret = i2c_add_adapter(&priv->ddc);
	KUNIT_ASSERT_EQ(test, ret, 0);

	ret = kunit_add_action_or_reset(test, i2c_del_adapter_wrapper, &priv->ddc);
	KUNIT_ASSERT_EQ(test, ret, 0);

	test->priv = priv;
	return 0;
}

/*
 * Test that the registration of a bog standard connector works as
 * expected and doesn't report any error.
 */
static void drm_test_drmm_connector_init(struct kunit *test)
{
	struct drm_connector_init_priv *priv = test->priv;
	int ret;

	ret = drmm_connector_init(&priv->drm, &priv->connector,
				  &dummy_funcs,
				  DRM_MODE_CONNECTOR_HDMIA,
				  &priv->ddc);
	KUNIT_EXPECT_EQ(test, ret, 0);
}

/*
 * Test that the registration of a connector without a DDC adapter
 * doesn't report any error.
 */
static void drm_test_drmm_connector_init_null_ddc(struct kunit *test)
{
	struct drm_connector_init_priv *priv = test->priv;
	int ret;

	ret = drmm_connector_init(&priv->drm, &priv->connector,
				  &dummy_funcs,
				  DRM_MODE_CONNECTOR_HDMIA,
				  NULL);
	KUNIT_EXPECT_EQ(test, ret, 0);
}

/*
 * Test that the registration of a connector succeeds for all possible
 * connector types.
 */
static void drm_test_drmm_connector_init_type_valid(struct kunit *test)
{
	struct drm_connector_init_priv *priv = test->priv;
	unsigned int connector_type = *(unsigned int *)test->param_value;
	int ret;

	ret = drmm_connector_init(&priv->drm, &priv->connector,
				  &dummy_funcs,
				  connector_type,
				  &priv->ddc);
	KUNIT_EXPECT_EQ(test, ret, 0);
}

static const unsigned int drm_connector_init_type_valid_tests[] = {
	DRM_MODE_CONNECTOR_Unknown,
	DRM_MODE_CONNECTOR_VGA,
	DRM_MODE_CONNECTOR_DVII,
	DRM_MODE_CONNECTOR_DVID,
	DRM_MODE_CONNECTOR_DVIA,
	DRM_MODE_CONNECTOR_Composite,
	DRM_MODE_CONNECTOR_SVIDEO,
	DRM_MODE_CONNECTOR_LVDS,
	DRM_MODE_CONNECTOR_Component,
	DRM_MODE_CONNECTOR_9PinDIN,
	DRM_MODE_CONNECTOR_DisplayPort,
	DRM_MODE_CONNECTOR_HDMIA,
	DRM_MODE_CONNECTOR_HDMIB,
	DRM_MODE_CONNECTOR_TV,
	DRM_MODE_CONNECTOR_eDP,
	DRM_MODE_CONNECTOR_VIRTUAL,
	DRM_MODE_CONNECTOR_DSI,
	DRM_MODE_CONNECTOR_DPI,
	DRM_MODE_CONNECTOR_WRITEBACK,
	DRM_MODE_CONNECTOR_SPI,
	DRM_MODE_CONNECTOR_USB,
};

static void drm_connector_init_type_desc(const unsigned int *type, char *desc)
{
	sprintf(desc, "%s", drm_get_connector_type_name(*type));
}

KUNIT_ARRAY_PARAM(drm_connector_init_type_valid,
		  drm_connector_init_type_valid_tests,
		  drm_connector_init_type_desc);

static struct kunit_case drmm_connector_init_tests[] = {
	KUNIT_CASE(drm_test_drmm_connector_init),
	KUNIT_CASE(drm_test_drmm_connector_init_null_ddc),
	KUNIT_CASE_PARAM(drm_test_drmm_connector_init_type_valid,
			 drm_connector_init_type_valid_gen_params),
	{ }
};

static struct kunit_suite drmm_connector_init_test_suite = {
	.name = "drmm_connector_init",
	.init = drm_test_connector_init,
	.test_cases = drmm_connector_init_tests,
};

static const struct drm_connector_funcs dummy_dynamic_init_funcs = {
	.atomic_destroy_state	= drm_atomic_helper_connector_destroy_state,
	.atomic_duplicate_state	= drm_atomic_helper_connector_duplicate_state,
	.reset			= drm_atomic_helper_connector_reset,
	.destroy		= drm_connector_cleanup,
};

/*
 * Test that the initialization of a bog standard dynamic connector works
 * as expected and doesn't report any error.
 */
static void drm_test_drm_connector_dynamic_init(struct kunit *test)
{
	struct drm_connector_init_priv *priv = test->priv;
	struct drm_connector *connector = &priv->connector;
	int ret;

	ret = drm_connector_dynamic_init(&priv->drm, connector,
					 &dummy_dynamic_init_funcs,
					 DRM_MODE_CONNECTOR_DisplayPort,
					 &priv->ddc);
	KUNIT_ASSERT_EQ(test, ret, 0);
}

static void drm_test_connector_dynamic_init_cleanup(struct kunit *test)
{
	struct drm_connector_init_priv *priv = test->priv;
	struct drm_connector *connector = &priv->connector;

	drm_connector_cleanup(connector);
}

/*
 * Test that the initialization of a dynamic connector without a DDC adapter
 * doesn't report any error.
 */
static void drm_test_drm_connector_dynamic_init_null_ddc(struct kunit *test)
{
	struct drm_connector_init_priv *priv = test->priv;
	struct drm_connector *connector = &priv->connector;
	int ret;

	ret = drm_connector_dynamic_init(&priv->drm, connector,
					 &dummy_dynamic_init_funcs,
					 DRM_MODE_CONNECTOR_DisplayPort,
					 NULL);
	KUNIT_ASSERT_EQ(test, ret, 0);
}

/*
 * Test that the initialization of a dynamic connector doesn't add the
 * connector to the connector list.
 */
static void drm_test_drm_connector_dynamic_init_not_added(struct kunit *test)
{
	struct drm_connector_init_priv *priv = test->priv;
	struct drm_connector *connector = &priv->connector;
	int ret;

	ret = drm_connector_dynamic_init(&priv->drm, connector,
					 &dummy_dynamic_init_funcs,
					 DRM_MODE_CONNECTOR_DisplayPort,
					 &priv->ddc);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_ASSERT_PTR_EQ(test, connector->head.next, &connector->head);
}

static void test_connector_property(struct kunit *test,
				    struct drm_connector *connector,
				    const struct drm_property *expected_prop)
{
	struct drm_property *prop;
	uint64_t val;
	int ret;

	KUNIT_ASSERT_NOT_NULL(test, expected_prop);
	prop = drm_mode_obj_find_prop_id(&connector->base, expected_prop->base.id);
	KUNIT_ASSERT_PTR_EQ_MSG(test, prop, expected_prop,
				"Can't find property %s", expected_prop->name);

	ret = drm_object_property_get_default_value(&connector->base, prop, &val);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, val, 0);

	/* TODO: Check property value in the connector state. */
}

/*
 * Test that the initialization of a dynamic connector adds all the expected
 * properties to it.
 */
static void drm_test_drm_connector_dynamic_init_properties(struct kunit *test)
{
	struct drm_connector_init_priv *priv = test->priv;
	struct drm_connector *connector = &priv->connector;
	struct drm_mode_config *config = &priv->drm.mode_config;
	const struct drm_property *props[] = {
		config->edid_property,
		config->dpms_property,
		config->link_status_property,
		config->non_desktop_property,
		config->tile_property,
		config->prop_crtc_id,
	};
	int ret;
	int i;

	ret = drm_connector_dynamic_init(&priv->drm, connector,
					 &dummy_dynamic_init_funcs,
					 DRM_MODE_CONNECTOR_DisplayPort,
					 &priv->ddc);
	KUNIT_ASSERT_EQ(test, ret, 0);

	for (i = 0; i < ARRAY_SIZE(props); i++)
		test_connector_property(test, connector, props[i]);
}

/*
 * Test that the initialization of a dynamic connector succeeds for all
 * possible connector types.
 */
static void drm_test_drm_connector_dynamic_init_type_valid(struct kunit *test)
{
	struct drm_connector_init_priv *priv = test->priv;
	struct drm_connector *connector = &priv->connector;
	unsigned int connector_type = *(unsigned int *)test->param_value;
	int ret;

	ret = drm_connector_dynamic_init(&priv->drm, connector,
					 &dummy_dynamic_init_funcs,
					 connector_type,
					 &priv->ddc);
	KUNIT_ASSERT_EQ(test, ret, 0);
}

/*
 * Test that the initialization of a dynamic connector sets the expected name
 * for it for all possible connector types.
 */
static void drm_test_drm_connector_dynamic_init_name(struct kunit *test)
{
	struct drm_connector_init_priv *priv = test->priv;
	struct drm_connector *connector = &priv->connector;
	unsigned int connector_type = *(unsigned int *)test->param_value;
	char expected_name[128];
	int ret;

	ret = drm_connector_dynamic_init(&priv->drm, connector,
					 &dummy_dynamic_init_funcs,
					 connector_type,
					 &priv->ddc);
	KUNIT_ASSERT_EQ(test, ret, 0);

	snprintf(expected_name, sizeof(expected_name), "%s-%d",
		 drm_get_connector_type_name(connector_type), connector->connector_type_id);
	KUNIT_ASSERT_STREQ(test, connector->name, expected_name);
}

static struct kunit_case drm_connector_dynamic_init_tests[] = {
	KUNIT_CASE(drm_test_drm_connector_dynamic_init),
	KUNIT_CASE(drm_test_drm_connector_dynamic_init_null_ddc),
	KUNIT_CASE(drm_test_drm_connector_dynamic_init_not_added),
	KUNIT_CASE(drm_test_drm_connector_dynamic_init_properties),
	KUNIT_CASE_PARAM(drm_test_drm_connector_dynamic_init_type_valid,
			 drm_connector_init_type_valid_gen_params),
	KUNIT_CASE_PARAM(drm_test_drm_connector_dynamic_init_name,
			 drm_connector_init_type_valid_gen_params),
	{}
};

static struct kunit_suite drm_connector_dynamic_init_test_suite = {
	.name = "drm_connector_dynamic_init",
	.init = drm_test_connector_init,
	.exit = drm_test_connector_dynamic_init_cleanup,
	.test_cases = drm_connector_dynamic_init_tests,
};

static int drm_test_connector_dynamic_register_early_init(struct kunit *test)
{
	struct drm_connector_init_priv *priv;
	int ret;

	ret = drm_test_connector_init(test);
	KUNIT_ASSERT_EQ(test, ret, 0);

	priv = test->priv;

	ret = drm_connector_dynamic_init(&priv->drm, &priv->connector,
					 &dummy_dynamic_init_funcs,
					 DRM_MODE_CONNECTOR_DisplayPort,
					 &priv->ddc);
	KUNIT_ASSERT_EQ(test, ret, 0);

	return 0;
}

static void drm_test_connector_dynamic_register_early_cleanup(struct kunit *test)
{
	struct drm_connector_init_priv *priv = test->priv;
	struct drm_connector *connector = &priv->connector;

	drm_connector_unregister(connector);
	drm_connector_put(connector);
}

/*
 * Test that registration of a dynamic connector adds it to the connector list.
 */
static void drm_test_drm_connector_dynamic_register_early_on_list(struct kunit *test)
{
	struct drm_connector_init_priv *priv = test->priv;
	struct drm_connector *connector = &priv->connector;
	int ret;

	KUNIT_ASSERT_TRUE(test, list_empty(&connector->head));

	ret = drm_connector_dynamic_register(connector);
	KUNIT_ASSERT_EQ(test, ret, 0);

	KUNIT_ASSERT_PTR_EQ(test, connector->head.next, &priv->drm.mode_config.connector_list);
}

/*
 * Test that the registration of a dynamic connector before the drm device is
 * registered results in deferring the connector's user interface registration.
 */
static void drm_test_drm_connector_dynamic_register_early_defer(struct kunit *test)
{
	struct drm_connector_init_priv *priv = test->priv;
	struct drm_connector *connector = &priv->connector;
	int ret;

	ret = drm_connector_dynamic_register(connector);
	KUNIT_ASSERT_EQ(test, ret, 0);

	KUNIT_ASSERT_EQ(test, connector->registration_state, DRM_CONNECTOR_INITIALIZING);
}

/*
 * Test that the registration of a dynamic connector fails, if this is done before
 * the connector is initialized.
 */
static void drm_test_drm_connector_dynamic_register_early_no_init(struct kunit *test)
{
	struct drm_connector *connector;
	int ret;

	connector = kunit_kzalloc(test, sizeof(*connector), GFP_KERNEL); /* auto freed */
	KUNIT_ASSERT_NOT_NULL(test, connector);

	ret = drm_connector_dynamic_register(connector);
	KUNIT_ASSERT_EQ(test, ret, -EINVAL);
}

/*
 * Test that the registration of a dynamic connector before the drm device is
 * registered results in deferring adding a mode object for the connector.
 */
static void drm_test_drm_connector_dynamic_register_early_no_mode_object(struct kunit *test)
{
	struct drm_connector_init_priv *priv = test->priv;
	struct drm_connector *connector = &priv->connector;
	struct drm_connector *tmp_connector;
	int ret;

	ret = drm_connector_dynamic_register(&priv->connector);
	KUNIT_ASSERT_EQ(test, ret, 0);

	tmp_connector = drm_connector_lookup(connector->dev, NULL, connector->base.id);
	KUNIT_ASSERT_NULL(test, tmp_connector);
}

static struct kunit_case drm_connector_dynamic_register_early_tests[] = {
	KUNIT_CASE(drm_test_drm_connector_dynamic_register_early_on_list),
	KUNIT_CASE(drm_test_drm_connector_dynamic_register_early_defer),
	KUNIT_CASE(drm_test_drm_connector_dynamic_register_early_no_init),
	KUNIT_CASE(drm_test_drm_connector_dynamic_register_early_no_mode_object),
	{ }
};

static struct kunit_suite drm_connector_dynamic_register_early_test_suite = {
	.name = "drm_connector_dynamic_register_early",
	.init = drm_test_connector_dynamic_register_early_init,
	.exit = drm_test_connector_dynamic_register_early_cleanup,
	.test_cases = drm_connector_dynamic_register_early_tests,
};

static int drm_test_connector_dynamic_register_init(struct kunit *test)
{
	struct drm_connector_init_priv *priv;
	int ret;

	ret = drm_test_connector_dynamic_register_early_init(test);
	KUNIT_ASSERT_EQ(test, ret, 0);

	priv = test->priv;

	ret = drm_dev_register(priv->connector.dev, 0);
	KUNIT_ASSERT_EQ(test, ret, 0);

	return 0;
}

static void drm_test_connector_dynamic_register_cleanup(struct kunit *test)
{
	struct drm_connector_init_priv *priv = test->priv;
	struct drm_device *dev = priv->connector.dev;

	drm_connector_unregister(&priv->connector);
	drm_connector_put(&priv->connector);

	drm_dev_unregister(dev);

	drm_test_connector_dynamic_register_early_cleanup(test);
}

static void drm_test_drm_connector_dynamic_register_on_list(struct kunit *test)
{
	struct drm_connector_init_priv *priv = test->priv;
	int ret;

	KUNIT_ASSERT_TRUE(test, list_empty(&priv->connector.head));

	ret = drm_connector_dynamic_register(&priv->connector);
	KUNIT_ASSERT_EQ(test, ret, 0);

	KUNIT_ASSERT_PTR_EQ(test, priv->connector.head.next, &priv->drm.mode_config.connector_list);
}

/*
 * Test that the registration of a dynamic connector doesn't get deferred if
 * this is done after the drm device is registered.
 */
static void drm_test_drm_connector_dynamic_register_no_defer(struct kunit *test)
{
	struct drm_connector_init_priv *priv = test->priv;
	int ret;

	KUNIT_ASSERT_EQ(test, priv->connector.registration_state, DRM_CONNECTOR_INITIALIZING);

	ret = drm_connector_dynamic_register(&priv->connector);
	KUNIT_ASSERT_EQ(test, ret, 0);

	KUNIT_ASSERT_EQ(test, priv->connector.registration_state, DRM_CONNECTOR_REGISTERED);
}

/*
 * Test that the registration of a dynamic connector fails if this is done after the
 * drm device is registered, but before the connector is initialized.
 */
static void drm_test_drm_connector_dynamic_register_no_init(struct kunit *test)
{
	struct drm_connector *connector;
	int ret;

	connector = kunit_kzalloc(test, sizeof(*connector), GFP_KERNEL); /* auto freed */
	KUNIT_ASSERT_NOT_NULL(test, connector);

	ret = drm_connector_dynamic_register(connector);
	KUNIT_ASSERT_EQ(test, ret, -EINVAL);
}

/*
 * Test that the registration of a dynamic connector after the drm device is
 * registered adds the mode object for the connector.
 */
static void drm_test_drm_connector_dynamic_register_mode_object(struct kunit *test)
{
	struct drm_connector_init_priv *priv = test->priv;
	struct drm_connector *connector = &priv->connector;
	struct drm_connector *tmp_connector;
	int ret;

	tmp_connector = drm_connector_lookup(connector->dev, NULL, connector->base.id);
	KUNIT_ASSERT_NULL(test, tmp_connector);

	ret = drm_connector_dynamic_register(&priv->connector);
	KUNIT_ASSERT_EQ(test, ret, 0);

	tmp_connector = drm_connector_lookup(connector->dev, NULL, connector->base.id);
	KUNIT_ASSERT_PTR_EQ(test, tmp_connector, connector);
}

/*
 * Test that the registration of a dynamic connector after the drm device is
 * registered adds the connector to sysfs.
 */
static void drm_test_drm_connector_dynamic_register_sysfs(struct kunit *test)
{
	struct drm_connector_init_priv *priv = test->priv;
	struct drm_connector *connector = &priv->connector;
	int ret;

	KUNIT_ASSERT_NULL(test, connector->kdev);

	ret = drm_connector_dynamic_register(connector);
	KUNIT_ASSERT_EQ(test, ret, 0);

	KUNIT_ASSERT_NOT_NULL(test, connector->kdev);
}

/*
 * Test that the registration of a dynamic connector after the drm device is
 * registered sets the connector's sysfs name as expected.
 */
static void drm_test_drm_connector_dynamic_register_sysfs_name(struct kunit *test)
{
	struct drm_connector_init_priv *priv = test->priv;
	struct drm_connector *connector = &priv->connector;
	char expected_name[128];
	int ret;

	ret = drm_connector_dynamic_register(connector);
	KUNIT_ASSERT_EQ(test, ret, 0);

	snprintf(expected_name, sizeof(expected_name), "card%d-%s",
		 connector->dev->primary->index, connector->name);

	KUNIT_ASSERT_STREQ(test, dev_name(connector->kdev), expected_name);
}

/*
 * Test that the registration of a dynamic connector after the drm device is
 * registered adds the connector to debugfs.
 */
static void drm_test_drm_connector_dynamic_register_debugfs(struct kunit *test)
{
	struct drm_connector_init_priv *priv = test->priv;
	int ret;

	KUNIT_ASSERT_NULL(test, priv->connector.debugfs_entry);

	ret = drm_connector_dynamic_register(&priv->connector);
	KUNIT_ASSERT_EQ(test, ret, 0);

	if (IS_ENABLED(CONFIG_DEBUG_FS))
		KUNIT_ASSERT_NOT_NULL(test, priv->connector.debugfs_entry);
	else
		KUNIT_ASSERT_NULL(test, priv->connector.debugfs_entry);
}

static struct kunit_case drm_connector_dynamic_register_tests[] = {
	KUNIT_CASE(drm_test_drm_connector_dynamic_register_on_list),
	KUNIT_CASE(drm_test_drm_connector_dynamic_register_no_defer),
	KUNIT_CASE(drm_test_drm_connector_dynamic_register_no_init),
	KUNIT_CASE(drm_test_drm_connector_dynamic_register_mode_object),
	KUNIT_CASE(drm_test_drm_connector_dynamic_register_sysfs),
	KUNIT_CASE(drm_test_drm_connector_dynamic_register_sysfs_name),
	KUNIT_CASE(drm_test_drm_connector_dynamic_register_debugfs),
	{ }
};

static struct kunit_suite drm_connector_dynamic_register_test_suite = {
	.name = "drm_connector_dynamic_register",
	.init = drm_test_connector_dynamic_register_init,
	.exit = drm_test_connector_dynamic_register_cleanup,
	.test_cases = drm_connector_dynamic_register_tests,
};

/*
 * Test that the registration of a bog standard connector works as
 * expected and doesn't report any error.
 */
static void drm_test_connector_hdmi_init_valid(struct kunit *test)
{
	struct drm_connector_init_priv *priv = test->priv;
	int ret;

	ret = drmm_connector_hdmi_init(&priv->drm, &priv->connector,
				       &dummy_funcs,
				       &dummy_hdmi_funcs,
				       DRM_MODE_CONNECTOR_HDMIA,
				       &priv->ddc);
	KUNIT_EXPECT_EQ(test, ret, 0);
}

/*
 * Test that the registration of a connector without a DDC adapter
 * doesn't report any error.
 */
static void drm_test_connector_hdmi_init_null_ddc(struct kunit *test)
{
	struct drm_connector_init_priv *priv = test->priv;
	int ret;

	ret = drmm_connector_hdmi_init(&priv->drm, &priv->connector,
				       &dummy_funcs,
				       &dummy_hdmi_funcs,
				       DRM_MODE_CONNECTOR_HDMIA,
				       NULL);
	KUNIT_EXPECT_EQ(test, ret, 0);
}

/*
 * Test that the registration of an HDMI connector with a NULL vendor
 * fails.
 */
static void drm_test_connector_hdmi_init_null_vendor(struct kunit *test)
{
	struct drm_connector_init_priv *priv = test->priv;
	int ret;

	priv->hdmi_funcs = dummy_hdmi_funcs;
	priv->hdmi_funcs.vendor = NULL;

	ret = drmm_connector_hdmi_init(&priv->drm, &priv->connector,
				       &dummy_funcs,
				       &priv->hdmi_funcs,
				       DRM_MODE_CONNECTOR_HDMIA,
				       &priv->ddc);
	KUNIT_EXPECT_LT(test, ret, 0);
}

/*
 * Test that the registration of an HDMI connector with a NULL product
 * fails.
 */
static void drm_test_connector_hdmi_init_null_product(struct kunit *test)
{
	struct drm_connector_init_priv *priv = test->priv;
	int ret;

	priv->hdmi_funcs = dummy_hdmi_funcs;
	priv->hdmi_funcs.product = NULL;

	ret = drmm_connector_hdmi_init(&priv->drm, &priv->connector,
				       &dummy_funcs,
				       &priv->hdmi_funcs,
				       DRM_MODE_CONNECTOR_HDMIA,
				       &priv->ddc);
	KUNIT_EXPECT_LT(test, ret, 0);
}

/*
 * Test that the registration of a connector with a valid, shorter than
 * the max length, product name succeeds.
 */
static void drm_test_connector_hdmi_init_product_valid(struct kunit *test)
{
	struct drm_connector_init_priv *priv = test->priv;
	int ret;

	priv->hdmi_funcs = dummy_hdmi_funcs;
	priv->hdmi_funcs.product = "Prod";

	KUNIT_ASSERT_LT(test, strlen(priv->hdmi_funcs.product),
			HDMI_SPD_INFOFRAME_PRODUCT_LEN);

	ret = drmm_connector_hdmi_init(&priv->drm, &priv->connector,
				       &dummy_funcs,
				       &priv->hdmi_funcs,
				       DRM_MODE_CONNECTOR_HDMIA,
				       &priv->ddc);
	KUNIT_EXPECT_EQ(test, ret, 0);
}

/*
 * Test that the registration of a connector with a valid, at max
 * length, product name succeeds.
 */
static void drm_test_connector_hdmi_init_product_length_exact(struct kunit *test)
{
	struct drm_connector_init_priv *priv = test->priv;
	int ret;

	priv->hdmi_funcs = dummy_hdmi_funcs;
	priv->hdmi_funcs.product = "ProductProductPr";

	KUNIT_ASSERT_EQ(test, strlen(priv->hdmi_funcs.product),
			HDMI_SPD_INFOFRAME_PRODUCT_LEN);

	ret = drmm_connector_hdmi_init(&priv->drm, &priv->connector,
				       &dummy_funcs,
				       &priv->hdmi_funcs,
				       DRM_MODE_CONNECTOR_HDMIA,
				       &priv->ddc);
	KUNIT_EXPECT_EQ(test, ret, 0);
}

/*
 * Test that the registration of a connector with a product name larger
 * than the maximum length fails.
 */
static void drm_test_connector_hdmi_init_product_length_too_long(struct kunit *test)
{
	struct drm_connector_init_priv *priv = test->priv;
	int ret;

	priv->hdmi_funcs = dummy_hdmi_funcs;
	priv->hdmi_funcs.product = "ProductProductProduct";

	KUNIT_ASSERT_GT(test, strlen(priv->hdmi_funcs.product),
			HDMI_SPD_INFOFRAME_PRODUCT_LEN);

	ret = drmm_connector_hdmi_init(&priv->drm, &priv->connector,
				       &dummy_funcs,
				       &priv->hdmi_funcs,
				       DRM_MODE_CONNECTOR_HDMIA,
				       &priv->ddc);
	KUNIT_EXPECT_LT(test, ret, 0);
}

/*
 * Test that the registration of a connector with a vendor name smaller
 * than the maximum length succeeds.
 */
static void drm_test_connector_hdmi_init_vendor_valid(struct kunit *test)
{
	struct drm_connector_init_priv *priv = test->priv;
	int ret;

	priv->hdmi_funcs = dummy_hdmi_funcs;
	priv->hdmi_funcs.vendor = "Vend";

	KUNIT_ASSERT_LT(test, strlen(priv->hdmi_funcs.vendor),
			HDMI_SPD_INFOFRAME_VENDOR_LEN);

	ret = drmm_connector_hdmi_init(&priv->drm, &priv->connector,
				       &dummy_funcs,
				       &priv->hdmi_funcs,
				       DRM_MODE_CONNECTOR_HDMIA,
				       &priv->ddc);
	KUNIT_EXPECT_EQ(test, ret, 0);
}

/*
 * Test that the registration of a connector with a vendor name at the
 * maximum length succeeds.
 */
static void drm_test_connector_hdmi_init_vendor_length_exact(struct kunit *test)
{
	struct drm_connector_init_priv *priv = test->priv;
	int ret;

	priv->hdmi_funcs = dummy_hdmi_funcs;
	priv->hdmi_funcs.vendor = "VendorVe";

	KUNIT_ASSERT_EQ(test, strlen(priv->hdmi_funcs.vendor),
			HDMI_SPD_INFOFRAME_VENDOR_LEN);

	ret = drmm_connector_hdmi_init(&priv->drm, &priv->connector,
				       &dummy_funcs,
				       &priv->hdmi_funcs,
				       DRM_MODE_CONNECTOR_HDMIA,
				       &priv->ddc);
	KUNIT_EXPECT_EQ(test, ret, 0);
}

/*
 * Test that the registration of a connector with a vendor name larger
 * than the maximum length fails.
 */
static void drm_test_connector_hdmi_init_vendor_length_too_long(struct kunit *test)
{
	struct drm_connector_init_priv *priv = test->priv;
	int ret;

	priv->hdmi_funcs = dummy_hdmi_funcs;
	priv->hdmi_funcs.vendor = "VendorVendor";
	KUNIT_ASSERT_GT(test, strlen(priv->hdmi_funcs.vendor), HDMI_SPD_INFOFRAME_VENDOR_LEN);

	ret = drmm_connector_hdmi_init(&priv->drm, &priv->connector,
				       &dummy_funcs,
				       &priv->hdmi_funcs,
				       DRM_MODE_CONNECTOR_HDMIA,
				       &priv->ddc);
	KUNIT_EXPECT_LT(test, ret, 0);
}

/*
 * Test that the registration of a connector with an invalid maximum bpc
 * count fails.
 */
static void drm_test_connector_hdmi_init_bpc_invalid(struct kunit *test)
{
	struct drm_connector_init_priv *priv = test->priv;
	int ret;

	priv->hdmi_funcs = dummy_hdmi_funcs;
	priv->hdmi_funcs.max_bpc = 9;

	ret = drmm_connector_hdmi_init(&priv->drm, &priv->connector,
				       &dummy_funcs,
				       &priv->hdmi_funcs,
				       DRM_MODE_CONNECTOR_HDMIA,
				       &priv->ddc);
	KUNIT_EXPECT_LT(test, ret, 0);
}

/*
 * Test that the registration of a connector with a null maximum bpc
 * count fails.
 */
static void drm_test_connector_hdmi_init_bpc_null(struct kunit *test)
{
	struct drm_connector_init_priv *priv = test->priv;
	int ret;

	priv->hdmi_funcs = dummy_hdmi_funcs;
	priv->hdmi_funcs.max_bpc = 0;

	ret = drmm_connector_hdmi_init(&priv->drm, &priv->connector,
				       &dummy_funcs,
				       &priv->hdmi_funcs,
				       DRM_MODE_CONNECTOR_HDMIA,
				       &priv->ddc);
	KUNIT_EXPECT_LT(test, ret, 0);
}

/*
 * Test that the registration of a connector with a maximum bpc count of
 * 8 succeeds, registers the max bpc property, but doesn't register the
 * HDR output metadata one.
 */
static void drm_test_connector_hdmi_init_bpc_8(struct kunit *test)
{
	struct drm_connector_init_priv *priv = test->priv;
	struct drm_connector_state *state;
	struct drm_connector *connector = &priv->connector;
	struct drm_property *prop;
	uint64_t val;
	int ret;

	ret = drmm_connector_hdmi_init(&priv->drm, &priv->connector,
				       &dummy_funcs,
				       &dummy_hdmi_funcs,
				       DRM_MODE_CONNECTOR_HDMIA,
				       &priv->ddc);
	KUNIT_EXPECT_EQ(test, ret, 0);

	prop = connector->max_bpc_property;
	KUNIT_ASSERT_NOT_NULL(test, prop);
	KUNIT_EXPECT_NOT_NULL(test, drm_mode_obj_find_prop_id(&connector->base, prop->base.id));

	ret = drm_object_property_get_default_value(&connector->base, prop, &val);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, val, 8);

	state = connector->state;
	KUNIT_EXPECT_EQ(test, state->max_bpc, 8);
	KUNIT_EXPECT_EQ(test, state->max_requested_bpc, 8);

	prop = priv->drm.mode_config.hdr_output_metadata_property;
	KUNIT_ASSERT_NOT_NULL(test, prop);
	KUNIT_EXPECT_NULL(test, drm_mode_obj_find_prop_id(&connector->base, prop->base.id));
}

/*
 * Test that the registration of a connector with a maximum bpc count of
 * 10 succeeds and registers the max bpc and HDR output metadata
 * properties.
 */
static void drm_test_connector_hdmi_init_bpc_10(struct kunit *test)
{
	struct drm_connector_init_priv *priv = test->priv;
	struct drm_connector_state *state;
	struct drm_connector *connector = &priv->connector;
	struct drm_property *prop;
	uint64_t val;
	int ret;

	priv->hdmi_funcs = dummy_hdmi_funcs;
	priv->hdmi_funcs.max_bpc = 10;

	ret = drmm_connector_hdmi_init(&priv->drm, &priv->connector,
				       &dummy_funcs,
				       &priv->hdmi_funcs,
				       DRM_MODE_CONNECTOR_HDMIA,
				       &priv->ddc);
	KUNIT_EXPECT_EQ(test, ret, 0);

	prop = connector->max_bpc_property;
	KUNIT_ASSERT_NOT_NULL(test, prop);
	KUNIT_EXPECT_NOT_NULL(test, drm_mode_obj_find_prop_id(&connector->base, prop->base.id));

	ret = drm_object_property_get_default_value(&connector->base, prop, &val);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, val, 10);

	state = connector->state;
	KUNIT_EXPECT_EQ(test, state->max_bpc, 10);
	KUNIT_EXPECT_EQ(test, state->max_requested_bpc, 10);

	prop = priv->drm.mode_config.hdr_output_metadata_property;
	KUNIT_ASSERT_NOT_NULL(test, prop);
	KUNIT_EXPECT_NOT_NULL(test, drm_mode_obj_find_prop_id(&connector->base, prop->base.id));
}

/*
 * Test that the registration of a connector with a maximum bpc count of
 * 12 succeeds and registers the max bpc and HDR output metadata
 * properties.
 */
static void drm_test_connector_hdmi_init_bpc_12(struct kunit *test)
{
	struct drm_connector_init_priv *priv = test->priv;
	struct drm_connector_state *state;
	struct drm_connector *connector = &priv->connector;
	struct drm_property *prop;
	uint64_t val;
	int ret;

	priv->hdmi_funcs = dummy_hdmi_funcs;
	priv->hdmi_funcs.max_bpc = 12;

	ret = drmm_connector_hdmi_init(&priv->drm, &priv->connector,
				       &dummy_funcs,
				       &priv->hdmi_funcs,
				       DRM_MODE_CONNECTOR_HDMIA,
				       &priv->ddc);
	KUNIT_EXPECT_EQ(test, ret, 0);

	prop = connector->max_bpc_property;
	KUNIT_ASSERT_NOT_NULL(test, prop);
	KUNIT_EXPECT_NOT_NULL(test, drm_mode_obj_find_prop_id(&connector->base, prop->base.id));

	ret = drm_object_property_get_default_value(&connector->base, prop, &val);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, val, 12);

	state = connector->state;
	KUNIT_EXPECT_EQ(test, state->max_bpc, 12);
	KUNIT_EXPECT_EQ(test, state->max_requested_bpc, 12);

	prop = priv->drm.mode_config.hdr_output_metadata_property;
	KUNIT_ASSERT_NOT_NULL(test, prop);
	KUNIT_EXPECT_NOT_NULL(test, drm_mode_obj_find_prop_id(&connector->base, prop->base.id));
}

/*
 * Test that the registration of an HDMI connector with no supported
 * format fails.
 */
static void drm_test_connector_hdmi_init_formats_empty(struct kunit *test)
{
	struct drm_connector_init_priv *priv = test->priv;
	int ret;

	priv->hdmi_funcs = dummy_hdmi_funcs;
	priv->hdmi_funcs.supported_formats = 0;

	ret = drmm_connector_hdmi_init(&priv->drm, &priv->connector,
				       &dummy_funcs,
				       &priv->hdmi_funcs,
				       DRM_MODE_CONNECTOR_HDMIA,
				       &priv->ddc);
	KUNIT_EXPECT_LT(test, ret, 0);
}

/*
 * Test that the registration of an HDMI connector not listing RGB as a
 * supported format fails.
 */
static void drm_test_connector_hdmi_init_formats_no_rgb(struct kunit *test)
{
	struct drm_connector_init_priv *priv = test->priv;
	int ret;

	priv->hdmi_funcs = dummy_hdmi_funcs;
	priv->hdmi_funcs.supported_formats = BIT(DRM_OUTPUT_COLOR_FORMAT_YCBCR422);

	ret = drmm_connector_hdmi_init(&priv->drm, &priv->connector,
				       &dummy_funcs,
				       &priv->hdmi_funcs,
				       DRM_MODE_CONNECTOR_HDMIA,
				       &priv->ddc);
	KUNIT_EXPECT_LT(test, ret, 0);
}

struct drm_connector_hdmi_init_formats_yuv420_allowed_test {
	unsigned long supported_formats;
	bool yuv420_allowed;
	int expected_result;
};

#define YUV420_ALLOWED_TEST(_formats, _allowed, _result)			\
	{									\
		.supported_formats = BIT(DRM_OUTPUT_COLOR_FORMAT_RGB444) | (_formats),	\
		.yuv420_allowed = _allowed,					\
		.expected_result = _result,					\
	}

static const struct drm_connector_hdmi_init_formats_yuv420_allowed_test
drm_connector_hdmi_init_formats_yuv420_allowed_tests[] = {
	YUV420_ALLOWED_TEST(BIT(DRM_OUTPUT_COLOR_FORMAT_YCBCR420), true, 0),
	YUV420_ALLOWED_TEST(BIT(DRM_OUTPUT_COLOR_FORMAT_YCBCR420), false, -EINVAL),
	YUV420_ALLOWED_TEST(BIT(DRM_OUTPUT_COLOR_FORMAT_YCBCR422), true, -EINVAL),
	YUV420_ALLOWED_TEST(BIT(DRM_OUTPUT_COLOR_FORMAT_YCBCR422), false, 0),
};

static void
drm_connector_hdmi_init_formats_yuv420_allowed_desc(const struct drm_connector_hdmi_init_formats_yuv420_allowed_test *t,
						    char *desc)
{
	sprintf(desc, "supported_formats=0x%lx yuv420_allowed=%d",
		t->supported_formats, t->yuv420_allowed);
}

KUNIT_ARRAY_PARAM(drm_connector_hdmi_init_formats_yuv420_allowed,
		  drm_connector_hdmi_init_formats_yuv420_allowed_tests,
		  drm_connector_hdmi_init_formats_yuv420_allowed_desc);

/*
 * Test that the registration of an HDMI connector succeeds only when
 * the presence of YUV420 in the supported formats matches the value
 * of the ycbcr_420_allowed flag.
 */
static void drm_test_connector_hdmi_init_formats_yuv420_allowed(struct kunit *test)
{
	const struct drm_connector_hdmi_init_formats_yuv420_allowed_test *params;
	struct drm_connector_init_priv *priv = test->priv;
	int ret;

	params = test->param_value;
	priv->connector.ycbcr_420_allowed = params->yuv420_allowed;

	priv->hdmi_funcs = dummy_hdmi_funcs;
	priv->hdmi_funcs.supported_formats = params->supported_formats;

	ret = drmm_connector_hdmi_init(&priv->drm, &priv->connector,
				       &dummy_funcs,
				       &priv->hdmi_funcs,
				       DRM_MODE_CONNECTOR_HDMIA,
				       &priv->ddc);
	KUNIT_EXPECT_EQ(test, ret, params->expected_result);
}

/*
 * Test that the registration of an HDMI connector with an HDMI
 * connector type succeeds.
 */
static void drm_test_connector_hdmi_init_type_valid(struct kunit *test)
{
	struct drm_connector_init_priv *priv = test->priv;
	unsigned int connector_type = *(unsigned int *)test->param_value;
	int ret;

	ret = drmm_connector_hdmi_init(&priv->drm, &priv->connector,
				       &dummy_funcs,
				       &dummy_hdmi_funcs,
				       connector_type,
				       &priv->ddc);
	KUNIT_EXPECT_EQ(test, ret, 0);
}

static const unsigned int drm_connector_hdmi_init_type_valid_tests[] = {
	DRM_MODE_CONNECTOR_HDMIA,
	DRM_MODE_CONNECTOR_HDMIB,
};

static void drm_connector_hdmi_init_type_desc(const unsigned int *type, char *desc)
{
	sprintf(desc, "%s", drm_get_connector_type_name(*type));
}

KUNIT_ARRAY_PARAM(drm_connector_hdmi_init_type_valid,
		  drm_connector_hdmi_init_type_valid_tests,
		  drm_connector_hdmi_init_type_desc);

/*
 * Test that the registration of an HDMI connector with an !HDMI
 * connector type fails.
 */
static void drm_test_connector_hdmi_init_type_invalid(struct kunit *test)
{
	struct drm_connector_init_priv *priv = test->priv;
	unsigned int connector_type = *(unsigned int *)test->param_value;
	int ret;

	ret = drmm_connector_hdmi_init(&priv->drm, &priv->connector,
				       &dummy_funcs,
				       &dummy_hdmi_funcs,
				       connector_type,
				       &priv->ddc);
	KUNIT_EXPECT_LT(test, ret, 0);
}

static const unsigned int drm_connector_hdmi_init_type_invalid_tests[] = {
	DRM_MODE_CONNECTOR_Unknown,
	DRM_MODE_CONNECTOR_VGA,
	DRM_MODE_CONNECTOR_DVII,
	DRM_MODE_CONNECTOR_DVID,
	DRM_MODE_CONNECTOR_DVIA,
	DRM_MODE_CONNECTOR_Composite,
	DRM_MODE_CONNECTOR_SVIDEO,
	DRM_MODE_CONNECTOR_LVDS,
	DRM_MODE_CONNECTOR_Component,
	DRM_MODE_CONNECTOR_9PinDIN,
	DRM_MODE_CONNECTOR_DisplayPort,
	DRM_MODE_CONNECTOR_TV,
	DRM_MODE_CONNECTOR_eDP,
	DRM_MODE_CONNECTOR_VIRTUAL,
	DRM_MODE_CONNECTOR_DSI,
	DRM_MODE_CONNECTOR_DPI,
	DRM_MODE_CONNECTOR_WRITEBACK,
	DRM_MODE_CONNECTOR_SPI,
	DRM_MODE_CONNECTOR_USB,
};

KUNIT_ARRAY_PARAM(drm_connector_hdmi_init_type_invalid,
		  drm_connector_hdmi_init_type_invalid_tests,
		  drm_connector_hdmi_init_type_desc);

/*
 * Test that the registration of an HDMI connector without an explicit max TMDS
 * character rate being provided succeeds, and the connector limit is inferred
 * from the advertised HDMI specification version.
 */
struct drm_connector_hdmi_init_max_tmds_rate_inferred_case {
	const char *desc;
	enum hdmi_version ver;
	unsigned long long expected;
};

static void drm_test_connector_hdmi_init_max_tmds_rate_inferred(struct kunit *test)
{
	struct drm_connector_init_priv *priv = test->priv;
	const struct drm_connector_hdmi_init_max_tmds_rate_inferred_case *params =
		test->param_value;
	int ret;

	priv->hdmi_funcs = params->ver < HDMI_VERSION_2_0 ?
				dummy_hdmi_funcs : dummy_hdmi_funcs_scrambler;
	priv->hdmi_funcs.supported_hdmi_ver = params->ver;

	ret = drmm_connector_hdmi_init(&priv->drm, &priv->connector,
				       &dummy_funcs,
				       &priv->hdmi_funcs,
				       DRM_MODE_CONNECTOR_HDMIA,
				       &priv->ddc);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, priv->connector.hdmi.max_tmds_char_rate,
			params->expected);
}

static const struct drm_connector_hdmi_init_max_tmds_rate_inferred_case
drm_connector_hdmi_init_max_tmds_rate_inferred_tests[] = {
	{ "unknown",	HDMI_VERSION_UNKNOWN,	0 },
	{ "1.0",	HDMI_VERSION_1_0,	HDMI_1_0_TMDS_CHAR_RATE_MAX_HZ },
	{ "1.2",	HDMI_VERSION_1_2,	HDMI_1_0_TMDS_CHAR_RATE_MAX_HZ },
	{ "1.3",	HDMI_VERSION_1_3,	HDMI_1_3_TMDS_CHAR_RATE_MAX_HZ },
	{ "1.4",	HDMI_VERSION_1_4,	HDMI_1_3_TMDS_CHAR_RATE_MAX_HZ },
	{ "2.0",	HDMI_VERSION_2_0,	HDMI_2_0_TMDS_CHAR_RATE_MAX_HZ },
};

static void drm_connector_hdmi_init_max_tmds_rate_inferred_desc(
	const struct drm_connector_hdmi_init_max_tmds_rate_inferred_case *t,
	char *desc)
{
	strscpy(desc, t->desc, KUNIT_PARAM_DESC_SIZE);
}

KUNIT_ARRAY_PARAM(drm_connector_hdmi_init_max_tmds_rate_inferred,
		  drm_connector_hdmi_init_max_tmds_rate_inferred_tests,
		  drm_connector_hdmi_init_max_tmds_rate_inferred_desc);

/*
 * Test that the registration of an HDMI connector providing a max TMDS
 * character rate strictly below the version-inferred limit succeeds, and
 * the connector limit is overridden.
 */
static void drm_test_connector_hdmi_init_max_tmds_rate_override(struct kunit *test)
{
	struct drm_connector_init_priv *priv = test->priv;
	int ret;

	priv->hdmi_funcs = dummy_hdmi_funcs;
	priv->hdmi_funcs.supported_hdmi_ver = HDMI_VERSION_1_4;
	priv->hdmi_funcs.supported_tmds_char_rate = HDMI_1_0_TMDS_CHAR_RATE_MAX_HZ;

	ret = drmm_connector_hdmi_init(&priv->drm, &priv->connector,
				       &dummy_funcs,
				       &priv->hdmi_funcs,
				       DRM_MODE_CONNECTOR_HDMIA,
				       &priv->ddc);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, priv->connector.hdmi.max_tmds_char_rate,
			HDMI_1_0_TMDS_CHAR_RATE_MAX_HZ);
}

/*
 * Test that the registration of an HDMI connector providing a max TMDS
 * character rate equal to the version-inferred limit succeeds.
 */
static void drm_test_connector_hdmi_init_max_tmds_rate_at_limit(struct kunit *test)
{
	struct drm_connector_init_priv *priv = test->priv;
	int ret;

	priv->hdmi_funcs = dummy_hdmi_funcs;
	priv->hdmi_funcs.supported_hdmi_ver = HDMI_VERSION_1_2;
	priv->hdmi_funcs.supported_tmds_char_rate = HDMI_1_0_TMDS_CHAR_RATE_MAX_HZ;

	ret = drmm_connector_hdmi_init(&priv->drm, &priv->connector,
				       &dummy_funcs,
				       &priv->hdmi_funcs,
				       DRM_MODE_CONNECTOR_HDMIA,
				       &priv->ddc);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, priv->connector.hdmi.max_tmds_char_rate,
			HDMI_1_0_TMDS_CHAR_RATE_MAX_HZ);
}

/*
 * Test that the registration of an HDMI connector providing a max TMDS
 * character rate that exceeds the limit inferred from the advertised HDMI
 * specification version fails.
 */
static void drm_test_connector_hdmi_init_max_tmds_rate_off_limit(struct kunit *test)
{
	struct drm_connector_init_priv *priv = test->priv;
	int ret;

	priv->hdmi_funcs = dummy_hdmi_funcs;
	priv->hdmi_funcs.supported_hdmi_ver = HDMI_VERSION_1_2;
	priv->hdmi_funcs.supported_tmds_char_rate = HDMI_1_3_TMDS_CHAR_RATE_MAX_HZ;

	ret = drmm_connector_hdmi_init(&priv->drm, &priv->connector,
				       &dummy_funcs,
				       &priv->hdmi_funcs,
				       DRM_MODE_CONNECTOR_HDMIA,
				       &priv->ddc);
	KUNIT_EXPECT_LT(test, ret, 0);
}

/*
 * Test that the registration of an HDMI connector providing a non-zero max
 * TMDS character rate without an HDMI specification version fails, as the
 * version-inferred limit defaults to zero and any positive override would
 * exceed it.
 */
static void drm_test_connector_hdmi_init_max_tmds_rate_no_version(struct kunit *test)
{
	struct drm_connector_init_priv *priv = test->priv;
	int ret;

	priv->hdmi_funcs = dummy_hdmi_funcs;
	priv->hdmi_funcs.supported_hdmi_ver = HDMI_VERSION_UNKNOWN;
	priv->hdmi_funcs.supported_tmds_char_rate = HDMI_1_0_TMDS_CHAR_RATE_MAX_HZ;

	ret = drmm_connector_hdmi_init(&priv->drm, &priv->connector,
				       &dummy_funcs,
				       &priv->hdmi_funcs,
				       DRM_MODE_CONNECTOR_HDMIA,
				       &priv->ddc);
	KUNIT_EXPECT_LT(test, ret, 0);
}

/*
 * Test that the registration of an HDMI connector advertising HDMI 2.0 support
 * succeeds when the .scrambler_{enable,disable} callbacks are provided.
 */
static void drm_test_connector_hdmi_init_scrambler_valid(struct kunit *test)
{
	struct drm_connector_init_priv *priv = test->priv;
	int ret;

	ret = drmm_connector_hdmi_init(&priv->drm, &priv->connector,
				       &dummy_funcs,
				       &dummy_hdmi_funcs_scrambler,
				       DRM_MODE_CONNECTOR_HDMIA,
				       &priv->ddc);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_TRUE(test, drm_connector_hdmi_scrambler_supported(&priv->connector));
}

/*
 * Test that the registration of an HDMI connector not advertising HDMI 2.0
 * support succeeds, and the connector is not reported as scrambling capable.
 */
static void drm_test_connector_hdmi_init_scrambler_unsupported(struct kunit *test)
{
	struct drm_connector_init_priv *priv = test->priv;
	int ret;

	ret = drmm_connector_hdmi_init(&priv->drm, &priv->connector,
				       &dummy_funcs,
				       &dummy_hdmi_funcs,
				       DRM_MODE_CONNECTOR_HDMIA,
				       &priv->ddc);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_FALSE(test, drm_connector_hdmi_scrambler_supported(&priv->connector));
}

enum drm_connector_hdmi_scrambler_cb {
	SCRAMBLER_CALLBACK_ENABLE,
	SCRAMBLER_CALLBACK_DISABLE,
	SCRAMBLER_CALLBACK_ALL,
};

struct drm_connector_hdmi_init_scrambler_missing_cb_case {
	const char *desc;
	enum drm_connector_hdmi_scrambler_cb cb;
};

/*
 * Test that the registration of an HDMI connector advertising HDMI 2.0 support
 * fails when any single .scrambler_* callback is missing.
 */
static void drm_test_connector_hdmi_init_scrambler_missing_cb(struct kunit *test)
{
	const struct drm_connector_hdmi_init_scrambler_missing_cb_case *params = test->param_value;
	struct drm_connector_init_priv *priv = test->priv;
	int ret;

	priv->hdmi_funcs = dummy_hdmi_funcs_scrambler;

	switch (params->cb) {
	case SCRAMBLER_CALLBACK_ENABLE:
		priv->hdmi_funcs.scrambler_enable = NULL;
		break;
	case SCRAMBLER_CALLBACK_DISABLE:
		priv->hdmi_funcs.scrambler_disable = NULL;
		break;
	case SCRAMBLER_CALLBACK_ALL:
		priv->hdmi_funcs.scrambler_enable = NULL;
		priv->hdmi_funcs.scrambler_disable = NULL;
		break;
	}

	ret = drmm_connector_hdmi_init(&priv->drm, &priv->connector,
				       &dummy_funcs,
				       &priv->hdmi_funcs,
				       DRM_MODE_CONNECTOR_HDMIA,
				       &priv->ddc);
	KUNIT_EXPECT_LT(test, ret, 0);
}

static const struct drm_connector_hdmi_init_scrambler_missing_cb_case
drm_connector_hdmi_init_scrambler_missing_cb_tests[] = {
	{ "scrambler_enable",	SCRAMBLER_CALLBACK_ENABLE },
	{ "scrambler_disable",	SCRAMBLER_CALLBACK_DISABLE },
	{ "scrambler_*",	SCRAMBLER_CALLBACK_ALL },
};

static void
drm_connector_hdmi_init_scrambler_missing_cb_desc(const struct drm_connector_hdmi_init_scrambler_missing_cb_case *t,
						  char *desc)
{
	strscpy(desc, t->desc, KUNIT_PARAM_DESC_SIZE);
}

KUNIT_ARRAY_PARAM(drm_connector_hdmi_init_scrambler_missing_cb,
		  drm_connector_hdmi_init_scrambler_missing_cb_tests,
		  drm_connector_hdmi_init_scrambler_missing_cb_desc);

/*
 * Test that the registration of an HDMI connector advertising HDMI 2.1 support
 * succeeds when all the .frl_* callbacks are provided, and that the FRL
 * rate/lane limits are inferred from the HDMI specification.
 */
static void drm_test_connector_hdmi_init_frl_valid(struct kunit *test)
{
	struct drm_connector_init_priv *priv = test->priv;
	struct drm_connector_hdmi *hdmi = &priv->connector.hdmi;
	int ret;

	ret = drmm_connector_hdmi_init(&priv->drm, &priv->connector,
				       &dummy_funcs,
				       &dummy_hdmi_funcs_frl,
				       DRM_MODE_CONNECTOR_HDMIA,
				       &priv->ddc);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_TRUE(test, drm_connector_hdmi_frl_supported(&priv->connector));

	KUNIT_EXPECT_EQ(test, hdmi->min_frl_rate_per_lane,
			HDMI_2_1_FRL_LANE_RATE_MIN_GBPS);
	KUNIT_EXPECT_EQ(test, hdmi->max_frl_rate_per_lane,
			HDMI_2_1_FRL_LANE_RATE_MAX_GBPS);
	KUNIT_EXPECT_EQ(test, hdmi->min_frl_lanes, HDMI_2_1_FRL_LANE_COUNT_MIN);
	KUNIT_EXPECT_EQ(test, hdmi->max_frl_lanes, HDMI_2_1_FRL_LANE_COUNT_MAX);
}

/*
 * Test that the registration of an HDMI connector not advertising HDMI 2.1
 * support succeeds, and the connector is not reported as FRL capable.
 */
static void drm_test_connector_hdmi_init_frl_unsupported(struct kunit *test)
{
	struct drm_connector_init_priv *priv = test->priv;
	int ret;

	ret = drmm_connector_hdmi_init(&priv->drm, &priv->connector,
				       &dummy_funcs,
				       &dummy_hdmi_funcs_scrambler,
				       DRM_MODE_CONNECTOR_HDMIA,
				       &priv->ddc);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_FALSE(test, drm_connector_hdmi_frl_supported(&priv->connector));
}

enum drm_connector_hdmi_frl_callback {
	FRL_CALLBACK_CONFIGURE,
	FRL_CALLBACK_SET_LTP,
	FRL_CALLBACK_TX_START,
	FRL_CALLBACK_TX_STOP,
	FRL_CALLBACK_FALLBACK_TMDS,
	FRL_CALLBACK_ALL,
};

struct drm_connector_hdmi_init_frl_callback_case {
	const char *desc;
	enum drm_connector_hdmi_frl_callback cb;
};

/*
 * Test that the registration of an HDMI connector advertising HDMI 2.1 support
 * fails when any single .frl_* callback is missing.
 */
static void drm_test_connector_hdmi_init_frl_missing_callback(struct kunit *test)
{
	const struct drm_connector_hdmi_init_frl_callback_case *params = test->param_value;
	struct drm_connector_init_priv *priv = test->priv;
	int ret;

	priv->hdmi_funcs = dummy_hdmi_funcs_frl;

	switch (params->cb) {
	case FRL_CALLBACK_CONFIGURE:
		priv->hdmi_funcs.frl_configure = NULL;
		break;
	case FRL_CALLBACK_SET_LTP:
		priv->hdmi_funcs.frl_set_ltp = NULL;
		break;
	case FRL_CALLBACK_TX_START:
		priv->hdmi_funcs.frl_tx_start = NULL;
		break;
	case FRL_CALLBACK_TX_STOP:
		priv->hdmi_funcs.frl_tx_stop = NULL;
		break;
	case FRL_CALLBACK_FALLBACK_TMDS:
		priv->hdmi_funcs.frl_fallback_tmds = NULL;
		break;
	case FRL_CALLBACK_ALL:
		priv->hdmi_funcs.frl_configure = NULL;
		priv->hdmi_funcs.frl_set_ltp = NULL;
		priv->hdmi_funcs.frl_tx_start = NULL;
		priv->hdmi_funcs.frl_tx_stop = NULL;
		priv->hdmi_funcs.frl_fallback_tmds = NULL;
		break;
	}

	ret = drmm_connector_hdmi_init(&priv->drm, &priv->connector,
				       &dummy_funcs,
				       &priv->hdmi_funcs,
				       DRM_MODE_CONNECTOR_HDMIA,
				       &priv->ddc);
	KUNIT_EXPECT_LT(test, ret, 0);
}

static const struct drm_connector_hdmi_init_frl_callback_case
drm_connector_hdmi_init_frl_callback_tests[] = {
	{ "frl_configure",	FRL_CALLBACK_CONFIGURE },
	{ "frl_set_ltp",	FRL_CALLBACK_SET_LTP },
	{ "frl_tx_start",	FRL_CALLBACK_TX_START },
	{ "frl_tx_stop",	FRL_CALLBACK_TX_STOP },
	{ "frl_fallback_tmds",	FRL_CALLBACK_FALLBACK_TMDS },
	{ "frl_*",		FRL_CALLBACK_ALL },
};

static void
drm_connector_hdmi_init_frl_callback_desc(const struct drm_connector_hdmi_init_frl_callback_case *t,
					  char *desc)
{
	strscpy(desc, t->desc, KUNIT_PARAM_DESC_SIZE);
}

KUNIT_ARRAY_PARAM(drm_connector_hdmi_init_frl_missing_callback,
		  drm_connector_hdmi_init_frl_callback_tests,
		  drm_connector_hdmi_init_frl_callback_desc);

/*
 * Test that the registration of an HDMI connector providing its own FRL
 * rate/lane limits succeeds, and those limits are left untouched.
 */
static void drm_test_connector_hdmi_init_frl_limits_override(struct kunit *test)
{
	struct drm_connector_init_priv *priv = test->priv;
	struct drm_connector_hdmi *hdmi = &priv->connector.hdmi;
	int ret;

	hdmi->min_frl_rate_per_lane = 6;
	hdmi->min_frl_lanes = 4;
	hdmi->max_frl_rate_per_lane = 10;
	hdmi->max_frl_lanes = 4;

	ret = drmm_connector_hdmi_init(&priv->drm, &priv->connector,
				       &dummy_funcs,
				       &dummy_hdmi_funcs_frl,
				       DRM_MODE_CONNECTOR_HDMIA,
				       &priv->ddc);
	KUNIT_EXPECT_EQ(test, ret, 0);

	KUNIT_EXPECT_EQ(test, hdmi->min_frl_rate_per_lane, 6);
	KUNIT_EXPECT_EQ(test, hdmi->min_frl_lanes, 4);
	KUNIT_EXPECT_EQ(test, hdmi->max_frl_rate_per_lane, 10);
	KUNIT_EXPECT_EQ(test, hdmi->max_frl_lanes, 4);
}

/*
 * Test that the registration of an HDMI connector providing FRL rate/lane
 * limits fails when those do not describe a valid capability window.
 */
struct drm_connector_hdmi_init_frl_limits_case {
	const char *desc;
	u8 min_rate_per_lane;
	u8 min_lanes;
	u8 max_rate_per_lane;
	u8 max_lanes;
};

static void drm_test_connector_hdmi_init_frl_limits_invalid(struct kunit *test)
{
	const struct drm_connector_hdmi_init_frl_limits_case *params = test->param_value;
	struct drm_connector_init_priv *priv = test->priv;
	struct drm_connector_hdmi *hdmi = &priv->connector.hdmi;
	int ret;

	hdmi->min_frl_rate_per_lane = params->min_rate_per_lane;
	hdmi->min_frl_lanes = params->min_lanes;
	hdmi->max_frl_rate_per_lane = params->max_rate_per_lane;
	hdmi->max_frl_lanes = params->max_lanes;

	ret = drmm_connector_hdmi_init(&priv->drm, &priv->connector,
				       &dummy_funcs,
				       &dummy_hdmi_funcs_frl,
				       DRM_MODE_CONNECTOR_HDMIA,
				       &priv->ddc);
	KUNIT_EXPECT_LT(test, ret, 0);
}

static const struct drm_connector_hdmi_init_frl_limits_case
drm_connector_hdmi_init_frl_limits_invalid_tests[] = {
	{ "rate-max-below-min",	12, 4, 6, 4 },
	{ "lanes-max-below-min", 6, 4, 6, 3 },
	{ "min-config-invalid",	 3, 4, 12, 4 },
	{ "max-config-invalid",	 3, 3, 3, 4 },
};

static void
drm_connector_hdmi_init_frl_limits_desc(const struct drm_connector_hdmi_init_frl_limits_case *t,
					char *desc)
{
	strscpy(desc, t->desc, KUNIT_PARAM_DESC_SIZE);
}

KUNIT_ARRAY_PARAM(drm_connector_hdmi_init_frl_limits_invalid,
		  drm_connector_hdmi_init_frl_limits_invalid_tests,
		  drm_connector_hdmi_init_frl_limits_desc);

/*
 * Test that the registration of an HDMI connector advertising FRL support
 * succeeds when a maximum TxFFE level is paired with the callback used to
 * apply it.
 */
static void drm_test_connector_hdmi_init_frl_ffe_level_valid(struct kunit *test)
{
	struct drm_connector_init_priv *priv = test->priv;
	int ret;

	priv->hdmi_funcs = dummy_hdmi_funcs_frl;
	priv->hdmi_funcs.frl_set_ffe_level = accept_frl_set_ffe_level;
	priv->connector.hdmi.max_ffe_level = 3;

	ret = drmm_connector_hdmi_init(&priv->drm, &priv->connector,
				       &dummy_funcs,
				       &priv->hdmi_funcs,
				       DRM_MODE_CONNECTOR_HDMIA,
				       &priv->ddc);
	KUNIT_EXPECT_EQ(test, ret, 0);
}

/*
 * Test that the registration of an HDMI connector advertising FRL support
 * fails when a maximum TxFFE level is provided without the callback used to
 * apply it.
 */
static void drm_test_connector_hdmi_init_frl_ffe_level_no_callback(struct kunit *test)
{
	struct drm_connector_init_priv *priv = test->priv;
	int ret;

	priv->connector.hdmi.max_ffe_level = 3;

	ret = drmm_connector_hdmi_init(&priv->drm, &priv->connector,
				       &dummy_funcs,
				       &dummy_hdmi_funcs_frl,
				       DRM_MODE_CONNECTOR_HDMIA,
				       &priv->ddc);
	KUNIT_EXPECT_LT(test, ret, 0);
}

/*
 * Test that the registration of an HDMI connector advertising FRL support
 * fails when the TxFFE callback is provided without a maximum level.
 */
static void drm_test_connector_hdmi_init_frl_ffe_level_no_max(struct kunit *test)
{
	struct drm_connector_init_priv *priv = test->priv;
	int ret;

	priv->hdmi_funcs = dummy_hdmi_funcs_frl;
	priv->hdmi_funcs.frl_set_ffe_level = accept_frl_set_ffe_level;

	ret = drmm_connector_hdmi_init(&priv->drm, &priv->connector,
				       &dummy_funcs,
				       &priv->hdmi_funcs,
				       DRM_MODE_CONNECTOR_HDMIA,
				       &priv->ddc);
	KUNIT_EXPECT_LT(test, ret, 0);
}

static struct kunit_case drmm_connector_hdmi_init_tests[] = {
	KUNIT_CASE(drm_test_connector_hdmi_init_valid),
	KUNIT_CASE(drm_test_connector_hdmi_init_bpc_8),
	KUNIT_CASE(drm_test_connector_hdmi_init_bpc_10),
	KUNIT_CASE(drm_test_connector_hdmi_init_bpc_12),
	KUNIT_CASE(drm_test_connector_hdmi_init_bpc_invalid),
	KUNIT_CASE(drm_test_connector_hdmi_init_bpc_null),
	KUNIT_CASE(drm_test_connector_hdmi_init_formats_empty),
	KUNIT_CASE(drm_test_connector_hdmi_init_formats_no_rgb),
	KUNIT_CASE_PARAM(drm_test_connector_hdmi_init_formats_yuv420_allowed,
			 drm_connector_hdmi_init_formats_yuv420_allowed_gen_params),
	KUNIT_CASE(drm_test_connector_hdmi_init_null_ddc),
	KUNIT_CASE(drm_test_connector_hdmi_init_null_product),
	KUNIT_CASE(drm_test_connector_hdmi_init_null_vendor),
	KUNIT_CASE(drm_test_connector_hdmi_init_product_length_exact),
	KUNIT_CASE(drm_test_connector_hdmi_init_product_length_too_long),
	KUNIT_CASE(drm_test_connector_hdmi_init_product_valid),
	KUNIT_CASE(drm_test_connector_hdmi_init_vendor_length_exact),
	KUNIT_CASE(drm_test_connector_hdmi_init_vendor_length_too_long),
	KUNIT_CASE(drm_test_connector_hdmi_init_vendor_valid),
	KUNIT_CASE_PARAM(drm_test_connector_hdmi_init_type_valid,
			 drm_connector_hdmi_init_type_valid_gen_params),
	KUNIT_CASE_PARAM(drm_test_connector_hdmi_init_type_invalid,
			 drm_connector_hdmi_init_type_invalid_gen_params),
	KUNIT_CASE_PARAM(drm_test_connector_hdmi_init_max_tmds_rate_inferred,
			 drm_connector_hdmi_init_max_tmds_rate_inferred_gen_params),
	KUNIT_CASE(drm_test_connector_hdmi_init_max_tmds_rate_override),
	KUNIT_CASE(drm_test_connector_hdmi_init_max_tmds_rate_at_limit),
	KUNIT_CASE(drm_test_connector_hdmi_init_max_tmds_rate_off_limit),
	KUNIT_CASE(drm_test_connector_hdmi_init_max_tmds_rate_no_version),
	KUNIT_CASE(drm_test_connector_hdmi_init_scrambler_valid),
	KUNIT_CASE(drm_test_connector_hdmi_init_scrambler_unsupported),
	KUNIT_CASE_PARAM(drm_test_connector_hdmi_init_scrambler_missing_cb,
			 drm_connector_hdmi_init_scrambler_missing_cb_gen_params),
	KUNIT_CASE(drm_test_connector_hdmi_init_frl_valid),
	KUNIT_CASE(drm_test_connector_hdmi_init_frl_unsupported),
	KUNIT_CASE_PARAM(drm_test_connector_hdmi_init_frl_missing_callback,
			 drm_connector_hdmi_init_frl_missing_callback_gen_params),
	KUNIT_CASE(drm_test_connector_hdmi_init_frl_limits_override),
	KUNIT_CASE_PARAM(drm_test_connector_hdmi_init_frl_limits_invalid,
			 drm_connector_hdmi_init_frl_limits_invalid_gen_params),
	KUNIT_CASE(drm_test_connector_hdmi_init_frl_ffe_level_valid),
	KUNIT_CASE(drm_test_connector_hdmi_init_frl_ffe_level_no_callback),
	KUNIT_CASE(drm_test_connector_hdmi_init_frl_ffe_level_no_max),
	{ }
};

static struct kunit_suite drmm_connector_hdmi_init_test_suite = {
	.name = "drmm_connector_hdmi_init",
	.init = drm_test_connector_init,
	.test_cases = drmm_connector_hdmi_init_tests,
};

struct drm_get_tv_mode_from_name_test {
	const char *name;
	enum drm_connector_tv_mode expected_mode;
};

#define TV_MODE_NAME(_name, _mode)		\
	{					\
		.name = _name,			\
		.expected_mode = _mode,		\
	}

static void drm_test_get_tv_mode_from_name_valid(struct kunit *test)
{
	const struct drm_get_tv_mode_from_name_test *params = test->param_value;

	KUNIT_EXPECT_EQ(test,
			drm_get_tv_mode_from_name(params->name, strlen(params->name)),
			params->expected_mode);
}

static const
struct drm_get_tv_mode_from_name_test drm_get_tv_mode_from_name_valid_tests[] = {
	TV_MODE_NAME("NTSC", DRM_MODE_TV_MODE_NTSC),
	TV_MODE_NAME("NTSC-443", DRM_MODE_TV_MODE_NTSC_443),
	TV_MODE_NAME("NTSC-J", DRM_MODE_TV_MODE_NTSC_J),
	TV_MODE_NAME("PAL", DRM_MODE_TV_MODE_PAL),
	TV_MODE_NAME("PAL-M", DRM_MODE_TV_MODE_PAL_M),
	TV_MODE_NAME("PAL-N", DRM_MODE_TV_MODE_PAL_N),
	TV_MODE_NAME("SECAM", DRM_MODE_TV_MODE_SECAM),
	TV_MODE_NAME("Mono", DRM_MODE_TV_MODE_MONOCHROME),
};

static void
drm_get_tv_mode_from_name_valid_desc(const struct drm_get_tv_mode_from_name_test *t,
				     char *desc)
{
	sprintf(desc, "%s", t->name);
}

KUNIT_ARRAY_PARAM(drm_get_tv_mode_from_name_valid,
		  drm_get_tv_mode_from_name_valid_tests,
		  drm_get_tv_mode_from_name_valid_desc);

static void drm_test_get_tv_mode_from_name_truncated(struct kunit *test)
{
	const char *name = "NTS";
	int ret;

	ret = drm_get_tv_mode_from_name(name, strlen(name));
	KUNIT_EXPECT_LT(test, ret, 0);
};

static struct kunit_case drm_get_tv_mode_from_name_tests[] = {
	KUNIT_CASE_PARAM(drm_test_get_tv_mode_from_name_valid,
			 drm_get_tv_mode_from_name_valid_gen_params),
	KUNIT_CASE(drm_test_get_tv_mode_from_name_truncated),
	{ }
};

static struct kunit_suite drm_get_tv_mode_from_name_test_suite = {
	.name = "drm_get_tv_mode_from_name",
	.test_cases = drm_get_tv_mode_from_name_tests,
};

struct drm_hdmi_connector_get_broadcast_rgb_name_test {
	unsigned int kind;
	const char *expected_name;
};

#define BROADCAST_RGB_TEST(_kind, _name)	\
	{					\
		.kind = _kind,			\
		.expected_name = _name,		\
	}

static void drm_test_drm_hdmi_connector_get_broadcast_rgb_name(struct kunit *test)
{
	const struct drm_hdmi_connector_get_broadcast_rgb_name_test *params =
		test->param_value;

	KUNIT_EXPECT_STREQ(test,
			   drm_hdmi_connector_get_broadcast_rgb_name(params->kind),
			   params->expected_name);
}

static const
struct drm_hdmi_connector_get_broadcast_rgb_name_test
drm_hdmi_connector_get_broadcast_rgb_name_valid_tests[] = {
	BROADCAST_RGB_TEST(DRM_HDMI_BROADCAST_RGB_AUTO, "Automatic"),
	BROADCAST_RGB_TEST(DRM_HDMI_BROADCAST_RGB_FULL, "Full"),
	BROADCAST_RGB_TEST(DRM_HDMI_BROADCAST_RGB_LIMITED, "Limited 16:235"),
};

static void
drm_hdmi_connector_get_broadcast_rgb_name_valid_desc(const struct drm_hdmi_connector_get_broadcast_rgb_name_test *t,
						     char *desc)
{
	sprintf(desc, "%s", t->expected_name);
}

KUNIT_ARRAY_PARAM(drm_hdmi_connector_get_broadcast_rgb_name_valid,
		  drm_hdmi_connector_get_broadcast_rgb_name_valid_tests,
		  drm_hdmi_connector_get_broadcast_rgb_name_valid_desc);

static void drm_test_drm_hdmi_connector_get_broadcast_rgb_name_invalid(struct kunit *test)
{
	KUNIT_EXPECT_NULL(test, drm_hdmi_connector_get_broadcast_rgb_name(3));
};

static struct kunit_case drm_hdmi_connector_get_broadcast_rgb_name_tests[] = {
	KUNIT_CASE_PARAM(drm_test_drm_hdmi_connector_get_broadcast_rgb_name,
			 drm_hdmi_connector_get_broadcast_rgb_name_valid_gen_params),
	KUNIT_CASE(drm_test_drm_hdmi_connector_get_broadcast_rgb_name_invalid),
	{ }
};

static struct kunit_suite drm_hdmi_connector_get_broadcast_rgb_name_test_suite = {
	.name = "drm_hdmi_connector_get_broadcast_rgb_name",
	.test_cases = drm_hdmi_connector_get_broadcast_rgb_name_tests,
};

struct drm_hdmi_connector_get_output_format_name_test {
	unsigned int kind;
	const char *expected_name;
};

#define OUTPUT_FORMAT_TEST(_kind, _name)	\
	{					\
		.kind = _kind,			\
		.expected_name = _name,		\
	}

static void drm_test_drm_hdmi_connector_get_output_format_name(struct kunit *test)
{
	const struct drm_hdmi_connector_get_output_format_name_test *params =
		test->param_value;

	KUNIT_EXPECT_STREQ(test,
			   drm_hdmi_connector_get_output_format_name(params->kind),
			   params->expected_name);
}

static const
struct drm_hdmi_connector_get_output_format_name_test
drm_hdmi_connector_get_output_format_name_valid_tests[] = {
	OUTPUT_FORMAT_TEST(DRM_OUTPUT_COLOR_FORMAT_RGB444, "RGB"),
	OUTPUT_FORMAT_TEST(DRM_OUTPUT_COLOR_FORMAT_YCBCR420, "YUV 4:2:0"),
	OUTPUT_FORMAT_TEST(DRM_OUTPUT_COLOR_FORMAT_YCBCR422, "YUV 4:2:2"),
	OUTPUT_FORMAT_TEST(DRM_OUTPUT_COLOR_FORMAT_YCBCR444, "YUV 4:4:4"),
};

static void
drm_hdmi_connector_get_output_format_name_valid_desc(const struct drm_hdmi_connector_get_output_format_name_test *t,
						     char *desc)
{
	sprintf(desc, "%s", t->expected_name);
}

KUNIT_ARRAY_PARAM(drm_hdmi_connector_get_output_format_name_valid,
		  drm_hdmi_connector_get_output_format_name_valid_tests,
		  drm_hdmi_connector_get_output_format_name_valid_desc);

static void drm_test_drm_hdmi_connector_get_output_format_name_invalid(struct kunit *test)
{
	KUNIT_EXPECT_NULL(test, drm_hdmi_connector_get_output_format_name(4));
};

static struct kunit_case drm_hdmi_connector_get_output_format_name_tests[] = {
	KUNIT_CASE_PARAM(drm_test_drm_hdmi_connector_get_output_format_name,
			 drm_hdmi_connector_get_output_format_name_valid_gen_params),
	KUNIT_CASE(drm_test_drm_hdmi_connector_get_output_format_name_invalid),
	{ }
};

static struct kunit_suite drm_hdmi_connector_get_output_format_name_test_suite = {
	.name = "drm_hdmi_connector_get_output_format_name",
	.test_cases = drm_hdmi_connector_get_output_format_name_tests,
};

static void drm_test_drm_connector_attach_broadcast_rgb_property(struct kunit *test)
{
	struct drm_connector_init_priv *priv = test->priv;
	struct drm_connector *connector = &priv->connector;
	struct drm_property *prop;
	int ret;

	ret = drmm_connector_init(&priv->drm, connector,
				  &dummy_funcs,
				  DRM_MODE_CONNECTOR_HDMIA,
				  &priv->ddc);
	KUNIT_ASSERT_EQ(test, ret, 0);

	ret = drm_connector_attach_broadcast_rgb_property(connector);
	KUNIT_ASSERT_EQ(test, ret, 0);

	prop = connector->broadcast_rgb_property;
	KUNIT_ASSERT_NOT_NULL(test, prop);
	KUNIT_EXPECT_NOT_NULL(test, drm_mode_obj_find_prop_id(&connector->base, prop->base.id));
}

static void drm_test_drm_connector_attach_broadcast_rgb_property_hdmi_connector(struct kunit *test)
{
	struct drm_connector_init_priv *priv = test->priv;
	struct drm_connector *connector = &priv->connector;
	struct drm_property *prop;
	int ret;

	ret = drmm_connector_hdmi_init(&priv->drm, connector,
				       &dummy_funcs,
				       &dummy_hdmi_funcs,
				       DRM_MODE_CONNECTOR_HDMIA,
				       &priv->ddc);
	KUNIT_EXPECT_EQ(test, ret, 0);

	ret = drm_connector_attach_broadcast_rgb_property(connector);
	KUNIT_ASSERT_EQ(test, ret, 0);

	prop = connector->broadcast_rgb_property;
	KUNIT_ASSERT_NOT_NULL(test, prop);
	KUNIT_EXPECT_NOT_NULL(test, drm_mode_obj_find_prop_id(&connector->base, prop->base.id));
}

static struct kunit_case drm_connector_attach_broadcast_rgb_property_tests[] = {
	KUNIT_CASE(drm_test_drm_connector_attach_broadcast_rgb_property),
	KUNIT_CASE(drm_test_drm_connector_attach_broadcast_rgb_property_hdmi_connector),
	{ }
};

static struct kunit_suite drm_connector_attach_broadcast_rgb_property_test_suite = {
	.name = "drm_connector_attach_broadcast_rgb_property",
	.init = drm_test_connector_init,
	.test_cases = drm_connector_attach_broadcast_rgb_property_tests,
};

/*
 * Test that for a given mode, with 8bpc and an RGB output the TMDS
 * character rate is equal to the mode pixel clock.
 */
static void drm_test_drm_hdmi_compute_mode_clock_rgb(struct kunit *test)
{
	struct drm_connector_init_priv *priv = test->priv;
	const struct drm_display_mode *mode;
	unsigned long long rate;
	struct drm_device *drm = &priv->drm;

	mode = drm_kunit_display_mode_from_cea_vic(test, drm, 16);
	KUNIT_ASSERT_NOT_NULL(test, mode);

	KUNIT_ASSERT_FALSE(test, mode->flags & DRM_MODE_FLAG_DBLCLK);

	rate = drm_hdmi_compute_mode_clock(mode, 8, DRM_OUTPUT_COLOR_FORMAT_RGB444);
	KUNIT_ASSERT_GT(test, rate, 0);
	KUNIT_EXPECT_EQ(test, mode->clock * 1000ULL, rate);
}

/*
 * Test that for a given mode, with 10bpc and an RGB output the TMDS
 * character rate is equal to 1.25 times the mode pixel clock.
 */
static void drm_test_drm_hdmi_compute_mode_clock_rgb_10bpc(struct kunit *test)
{
	struct drm_connector_init_priv *priv = test->priv;
	const struct drm_display_mode *mode;
	unsigned long long rate;
	struct drm_device *drm = &priv->drm;

	mode = drm_kunit_display_mode_from_cea_vic(test, drm, 16);
	KUNIT_ASSERT_NOT_NULL(test, mode);

	KUNIT_ASSERT_FALSE(test, mode->flags & DRM_MODE_FLAG_DBLCLK);

	rate = drm_hdmi_compute_mode_clock(mode, 10, DRM_OUTPUT_COLOR_FORMAT_RGB444);
	KUNIT_ASSERT_GT(test, rate, 0);
	KUNIT_EXPECT_EQ(test, mode->clock * 1250, rate);
}

/*
 * Test that for the VIC-1 mode, with 10bpc and an RGB output the TMDS
 * character rate computation fails.
 */
static void drm_test_drm_hdmi_compute_mode_clock_rgb_10bpc_vic_1(struct kunit *test)
{
	struct drm_connector_init_priv *priv = test->priv;
	const struct drm_display_mode *mode;
	unsigned long long rate;
	struct drm_device *drm = &priv->drm;

	mode = drm_kunit_display_mode_from_cea_vic(test, drm, 1);
	KUNIT_ASSERT_NOT_NULL(test, mode);

	rate = drm_hdmi_compute_mode_clock(mode, 10, DRM_OUTPUT_COLOR_FORMAT_RGB444);
	KUNIT_EXPECT_EQ(test, rate, 0);
}

/*
 * Test that for a given mode, with 12bpc and an RGB output the TMDS
 * character rate is equal to 1.5 times the mode pixel clock.
 */
static void drm_test_drm_hdmi_compute_mode_clock_rgb_12bpc(struct kunit *test)
{
	struct drm_connector_init_priv *priv = test->priv;
	const struct drm_display_mode *mode;
	unsigned long long rate;
	struct drm_device *drm = &priv->drm;

	mode = drm_kunit_display_mode_from_cea_vic(test, drm, 16);
	KUNIT_ASSERT_NOT_NULL(test, mode);

	KUNIT_ASSERT_FALSE(test, mode->flags & DRM_MODE_FLAG_DBLCLK);

	rate = drm_hdmi_compute_mode_clock(mode, 12, DRM_OUTPUT_COLOR_FORMAT_RGB444);
	KUNIT_ASSERT_GT(test, rate, 0);
	KUNIT_EXPECT_EQ(test, mode->clock * 1500, rate);
}

/*
 * Test that for the VIC-1 mode, with 12bpc and an RGB output the TMDS
 * character rate computation fails.
 */
static void drm_test_drm_hdmi_compute_mode_clock_rgb_12bpc_vic_1(struct kunit *test)
{
	struct drm_connector_init_priv *priv = test->priv;
	const struct drm_display_mode *mode;
	unsigned long long rate;
	struct drm_device *drm = &priv->drm;

	mode = drm_kunit_display_mode_from_cea_vic(test, drm, 1);
	KUNIT_ASSERT_NOT_NULL(test, mode);

	rate = drm_hdmi_compute_mode_clock(mode, 12, DRM_OUTPUT_COLOR_FORMAT_RGB444);
	KUNIT_EXPECT_EQ(test, rate, 0);
}

/*
 * Test that for a mode with the pixel repetition flag, the TMDS
 * character rate is indeed double the mode pixel clock.
 */
static void drm_test_drm_hdmi_compute_mode_clock_rgb_double(struct kunit *test)
{
	struct drm_connector_init_priv *priv = test->priv;
	const struct drm_display_mode *mode;
	unsigned long long rate;
	struct drm_device *drm = &priv->drm;

	mode = drm_kunit_display_mode_from_cea_vic(test, drm, 6);
	KUNIT_ASSERT_NOT_NULL(test, mode);

	KUNIT_ASSERT_TRUE(test, mode->flags & DRM_MODE_FLAG_DBLCLK);

	rate = drm_hdmi_compute_mode_clock(mode, 8, DRM_OUTPUT_COLOR_FORMAT_RGB444);
	KUNIT_ASSERT_GT(test, rate, 0);
	KUNIT_EXPECT_EQ(test, (mode->clock * 1000ULL) * 2, rate);
}

/*
 * Test that the TMDS character rate computation for the VIC modes
 * explicitly listed in the spec as supporting YUV420 succeed and return
 * half the mode pixel clock.
 */
static void drm_test_connector_hdmi_compute_mode_clock_yuv420_valid(struct kunit *test)
{
	struct drm_connector_init_priv *priv = test->priv;
	const struct drm_display_mode *mode;
	struct drm_device *drm = &priv->drm;
	unsigned long long rate;
	unsigned int vic = *(unsigned int *)test->param_value;

	mode = drm_kunit_display_mode_from_cea_vic(test, drm, vic);
	KUNIT_ASSERT_NOT_NULL(test, mode);

	KUNIT_ASSERT_FALSE(test, mode->flags & DRM_MODE_FLAG_DBLCLK);

	rate = drm_hdmi_compute_mode_clock(mode, 8, DRM_OUTPUT_COLOR_FORMAT_YCBCR420);
	KUNIT_ASSERT_GT(test, rate, 0);
	KUNIT_EXPECT_EQ(test, (mode->clock * 1000ULL) / 2, rate);
}

static const unsigned int drm_hdmi_compute_mode_clock_yuv420_vic_valid_tests[] = {
	96, 97, 101, 102, 106, 107,
};

static void drm_hdmi_compute_mode_clock_yuv420_vic_desc(const unsigned int *vic, char *desc)
{
	sprintf(desc, "VIC %u", *vic);
}

KUNIT_ARRAY_PARAM(drm_hdmi_compute_mode_clock_yuv420_valid,
		  drm_hdmi_compute_mode_clock_yuv420_vic_valid_tests,
		  drm_hdmi_compute_mode_clock_yuv420_vic_desc);

/*
 * Test that for a given mode listed supporting it and an YUV420 output
 * with 10bpc, the TMDS character rate is equal to 0.625 times the mode
 * pixel clock.
 */
static void drm_test_connector_hdmi_compute_mode_clock_yuv420_10_bpc(struct kunit *test)
{
	struct drm_connector_init_priv *priv = test->priv;
	const struct drm_display_mode *mode;
	struct drm_device *drm = &priv->drm;
	unsigned int vic =
		drm_hdmi_compute_mode_clock_yuv420_vic_valid_tests[0];
	unsigned long long rate;

	mode = drm_kunit_display_mode_from_cea_vic(test, drm, vic);
	KUNIT_ASSERT_NOT_NULL(test, mode);

	KUNIT_ASSERT_FALSE(test, mode->flags & DRM_MODE_FLAG_DBLCLK);

	rate = drm_hdmi_compute_mode_clock(mode, 10, DRM_OUTPUT_COLOR_FORMAT_YCBCR420);
	KUNIT_ASSERT_GT(test, rate, 0);

	KUNIT_EXPECT_EQ(test, mode->clock * 625, rate);
}

/*
 * Test that for a given mode listed supporting it and an YUV420 output
 * with 12bpc, the TMDS character rate is equal to 0.75 times the mode
 * pixel clock.
 */
static void drm_test_connector_hdmi_compute_mode_clock_yuv420_12_bpc(struct kunit *test)
{
	struct drm_connector_init_priv *priv = test->priv;
	const struct drm_display_mode *mode;
	struct drm_device *drm = &priv->drm;
	unsigned int vic =
		drm_hdmi_compute_mode_clock_yuv420_vic_valid_tests[0];
	unsigned long long rate;

	mode = drm_kunit_display_mode_from_cea_vic(test, drm, vic);
	KUNIT_ASSERT_NOT_NULL(test, mode);

	KUNIT_ASSERT_FALSE(test, mode->flags & DRM_MODE_FLAG_DBLCLK);

	rate = drm_hdmi_compute_mode_clock(mode, 12, DRM_OUTPUT_COLOR_FORMAT_YCBCR420);
	KUNIT_ASSERT_GT(test, rate, 0);

	KUNIT_EXPECT_EQ(test, mode->clock * 750, rate);
}

/*
 * Test that for a given mode, the computation of the TMDS character
 * rate with 8bpc and a YUV422 output succeeds and returns a rate equal
 * to the mode pixel clock.
 */
static void drm_test_connector_hdmi_compute_mode_clock_yuv422_8_bpc(struct kunit *test)
{
	struct drm_connector_init_priv *priv = test->priv;
	const struct drm_display_mode *mode;
	struct drm_device *drm = &priv->drm;
	unsigned long long rate;

	mode = drm_kunit_display_mode_from_cea_vic(test, drm, 16);
	KUNIT_ASSERT_NOT_NULL(test, mode);

	KUNIT_ASSERT_FALSE(test, mode->flags & DRM_MODE_FLAG_DBLCLK);

	rate = drm_hdmi_compute_mode_clock(mode, 8, DRM_OUTPUT_COLOR_FORMAT_YCBCR422);
	KUNIT_ASSERT_GT(test, rate, 0);
	KUNIT_EXPECT_EQ(test, mode->clock * 1000, rate);
}

/*
 * Test that for a given mode, the computation of the TMDS character
 * rate with 10bpc and a YUV422 output succeeds and returns a rate equal
 * to the mode pixel clock.
 */
static void drm_test_connector_hdmi_compute_mode_clock_yuv422_10_bpc(struct kunit *test)
{
	struct drm_connector_init_priv *priv = test->priv;
	const struct drm_display_mode *mode;
	struct drm_device *drm = &priv->drm;
	unsigned long long rate;

	mode = drm_kunit_display_mode_from_cea_vic(test, drm, 16);
	KUNIT_ASSERT_NOT_NULL(test, mode);

	KUNIT_ASSERT_FALSE(test, mode->flags & DRM_MODE_FLAG_DBLCLK);

	rate = drm_hdmi_compute_mode_clock(mode, 10, DRM_OUTPUT_COLOR_FORMAT_YCBCR422);
	KUNIT_ASSERT_GT(test, rate, 0);
	KUNIT_EXPECT_EQ(test, mode->clock * 1000, rate);
}

/*
 * Test that for a given mode, the computation of the TMDS character
 * rate with 12bpc and a YUV422 output succeeds and returns a rate equal
 * to the mode pixel clock.
 */
static void drm_test_connector_hdmi_compute_mode_clock_yuv422_12_bpc(struct kunit *test)
{
	struct drm_connector_init_priv *priv = test->priv;
	const struct drm_display_mode *mode;
	struct drm_device *drm = &priv->drm;
	unsigned long long rate;

	mode = drm_kunit_display_mode_from_cea_vic(test, drm, 16);
	KUNIT_ASSERT_NOT_NULL(test, mode);

	KUNIT_ASSERT_FALSE(test, mode->flags & DRM_MODE_FLAG_DBLCLK);

	rate = drm_hdmi_compute_mode_clock(mode, 12, DRM_OUTPUT_COLOR_FORMAT_YCBCR422);
	KUNIT_ASSERT_GT(test, rate, 0);
	KUNIT_EXPECT_EQ(test, mode->clock * 1000, rate);
}

static struct kunit_case drm_hdmi_compute_mode_clock_tests[] = {
	KUNIT_CASE(drm_test_drm_hdmi_compute_mode_clock_rgb),
	KUNIT_CASE(drm_test_drm_hdmi_compute_mode_clock_rgb_10bpc),
	KUNIT_CASE(drm_test_drm_hdmi_compute_mode_clock_rgb_10bpc_vic_1),
	KUNIT_CASE(drm_test_drm_hdmi_compute_mode_clock_rgb_12bpc),
	KUNIT_CASE(drm_test_drm_hdmi_compute_mode_clock_rgb_12bpc_vic_1),
	KUNIT_CASE(drm_test_drm_hdmi_compute_mode_clock_rgb_double),
	KUNIT_CASE_PARAM(drm_test_connector_hdmi_compute_mode_clock_yuv420_valid,
			 drm_hdmi_compute_mode_clock_yuv420_valid_gen_params),
	KUNIT_CASE(drm_test_connector_hdmi_compute_mode_clock_yuv420_10_bpc),
	KUNIT_CASE(drm_test_connector_hdmi_compute_mode_clock_yuv420_12_bpc),
	KUNIT_CASE(drm_test_connector_hdmi_compute_mode_clock_yuv422_8_bpc),
	KUNIT_CASE(drm_test_connector_hdmi_compute_mode_clock_yuv422_10_bpc),
	KUNIT_CASE(drm_test_connector_hdmi_compute_mode_clock_yuv422_12_bpc),
	{ }
};

static struct kunit_suite drm_hdmi_compute_mode_clock_test_suite = {
	.name = "drm_test_connector_hdmi_compute_mode_clock",
	.init = drm_test_connector_init,
	.test_cases = drm_hdmi_compute_mode_clock_tests,
};

kunit_test_suites(
	&drmm_connector_hdmi_init_test_suite,
	&drmm_connector_init_test_suite,
	&drm_connector_dynamic_init_test_suite,
	&drm_connector_dynamic_register_early_test_suite,
	&drm_connector_dynamic_register_test_suite,
	&drm_connector_attach_broadcast_rgb_property_test_suite,
	&drm_get_tv_mode_from_name_test_suite,
	&drm_hdmi_compute_mode_clock_test_suite,
	&drm_hdmi_connector_get_broadcast_rgb_name_test_suite,
	&drm_hdmi_connector_get_output_format_name_test_suite
);

MODULE_AUTHOR("Maxime Ripard <maxime@cerno.tech>");
MODULE_DESCRIPTION("Kunit test for drm_modes functions");
MODULE_LICENSE("GPL");
