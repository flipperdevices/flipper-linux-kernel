// SPDX-License-Identifier: (GPL-2.0-only OR BSD-2-Clause)

#include <drm/clients/drm_client_setup.h>
#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_connector.h>
#include <drm/drm_damage_helper.h>
#include <drm/drm_drv.h>
#include <drm/drm_fb_dma_helper.h>
#include <drm/drm_fbdev_dma.h>
#include <drm/drm_format_helper.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem_atomic_helper.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_managed.h>
#include <drm/drm_modes.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_rect.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/spi/spi.h>

struct fo_device {
	struct drm_device drm;
	struct spi_device *spi;

	const struct drm_display_mode *mode;

	struct drm_crtc crtc;
	struct drm_plane plane;
	struct drm_encoder encoder;
	struct drm_connector connector;

	struct gpio_desc *active_gpio;

	u32 pitch;
	u32 tx_buffer_size;
	u8 *tx_buffer;
};

static inline struct fo_device *drm_to_fo_device(struct drm_device *drm)
{
	return container_of(drm, struct fo_device, drm);
}

DEFINE_DRM_GEM_DMA_FOPS(fo_fops);

static const struct drm_driver fo_drm_driver = {
	.driver_features	= DRIVER_GEM | DRIVER_MODESET | DRIVER_ATOMIC,
	.fops			= &fo_fops,
	DRM_GEM_DMA_DRIVER_OPS_VMAP,
	DRM_FBDEV_DMA_DRIVER_OPS,
	.name			= "flipper_one_display",
	.desc			= "Flipper One LCD screen",
	.major			= 1,
	.minor			= 0,
};

static void fo_set_tx_buffer_data(struct fo_device *fo,
				       struct drm_plane_state *plane_state)
{
	struct drm_shadow_plane_state *s_plane_state = to_drm_shadow_plane_state(plane_state);
	struct drm_framebuffer *fb = plane_state->fb;
	struct drm_rect clip;
	struct iosys_map *src = s_plane_state->data;
	struct iosys_map dst;

	if (drm_gem_fb_begin_cpu_access(fb, DMA_FROM_DEVICE))
		return;

	clip.x1 = 0;
	clip.x2 = fb->width;
	clip.y1 = 0;
	clip.y2 = fb->height;

	iosys_map_set_vaddr(&dst, fo->tx_buffer);
	drm_fb_xrgb8888_to_gray8(&dst, &fo->pitch, src, fb, &clip, &s_plane_state->fmtcnv_state);
	drm_gem_fb_end_cpu_access(fb, DMA_FROM_DEVICE);
}

static int fo_plane_atomic_check(struct drm_plane *plane, struct drm_atomic_state *state)
{
	struct drm_plane_state *plane_state = drm_atomic_get_new_plane_state(state, plane);
	struct fo_device *fo;
	struct drm_crtc_state *crtc_state;

	fo = container_of(plane, struct fo_device, plane);
	crtc_state = drm_atomic_get_new_crtc_state(state, &fo->crtc);

	return drm_atomic_helper_check_plane_state(plane_state, crtc_state,
						   DRM_PLANE_NO_SCALING,
						   DRM_PLANE_NO_SCALING,
						   false, false);
}

static void fo_plane_atomic_update(struct drm_plane *plane, struct drm_atomic_state *state)
{
	struct fo_device *fo = container_of(plane, struct fo_device, plane);
	struct drm_plane_state *plane_state = plane->state;

	if (!fo->crtc.state->active)
		return;

	/* Populate the transmit buffer with frame data */
	fo_set_tx_buffer_data(fo, plane_state);

	spi_write(fo->spi, fo->tx_buffer, fo->tx_buffer_size);
}

static const struct drm_plane_helper_funcs fo_plane_helper_funcs = {
	.prepare_fb = drm_gem_plane_helper_prepare_fb,
	.atomic_check = fo_plane_atomic_check,
	.atomic_update = fo_plane_atomic_update,
	DRM_GEM_SHADOW_PLANE_HELPER_FUNCS,
};

static bool fo_format_mod_supported(struct drm_plane *plane, u32 format, u64 modifier)
{
	return modifier == DRM_FORMAT_MOD_LINEAR;
}

static const struct drm_plane_funcs fo_plane_funcs = {
	.update_plane = drm_atomic_helper_update_plane,
	.disable_plane = drm_atomic_helper_disable_plane,
	.destroy = drm_plane_cleanup,
	DRM_GEM_SHADOW_PLANE_FUNCS,
	.format_mod_supported = fo_format_mod_supported,
};

static enum drm_mode_status fo_crtc_mode_valid(struct drm_crtc *crtc,
					       const struct drm_display_mode *mode)
{
	struct fo_device *fo = drm_to_fo_device(crtc->dev);

	return drm_crtc_helper_mode_valid_fixed(crtc, mode, fo->mode);
}

static int fo_crtc_check(struct drm_crtc *crtc, struct drm_atomic_state *state)
{
	struct drm_crtc_state *crtc_state = drm_atomic_get_new_crtc_state(state, crtc);
	int ret;

	if (!crtc_state->enable)
		goto out;

	ret = drm_atomic_helper_check_crtc_primary_plane(crtc_state);
	if (ret)
		return ret;

out:
	return drm_atomic_add_affected_planes(state, crtc);
}

static void fo_begin_frame(struct drm_crtc *crtc, struct drm_atomic_state *state)
{
	struct fo_device *fo = drm_to_fo_device(crtc->dev);

	if (fo->active_gpio)
		gpiod_set_value(fo->active_gpio, 1);
}

static void fo_end_frame(struct drm_crtc *crtc, struct drm_atomic_state *state)
{
	struct fo_device *fo = drm_to_fo_device(crtc->dev);

	if (fo->active_gpio)
		gpiod_set_value(fo->active_gpio, 0);
}

static const struct drm_crtc_helper_funcs fo_crtc_helper_funcs = {
	.mode_valid = fo_crtc_mode_valid,
	.atomic_check = fo_crtc_check,
	.atomic_begin = fo_begin_frame,
	.atomic_flush = fo_end_frame,
};

static const struct drm_crtc_funcs fo_crtc_funcs = {
	.reset = drm_atomic_helper_crtc_reset,
	.destroy = drm_crtc_cleanup,
	.set_config = drm_atomic_helper_set_config,
	.page_flip = drm_atomic_helper_page_flip,
	.atomic_duplicate_state = drm_atomic_helper_crtc_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_crtc_destroy_state,
};

static const struct drm_encoder_funcs fo_encoder_funcs = {
	.destroy = drm_encoder_cleanup,
};

static int fo_connector_get_modes(struct drm_connector *connector)
{
	struct fo_device *fo = drm_to_fo_device(connector->dev);

	return drm_connector_helper_get_modes_fixed(connector, fo->mode);
}

static const struct drm_connector_helper_funcs fo_connector_hfuncs = {
	.get_modes = fo_connector_get_modes,
};

static const struct drm_connector_funcs fo_connector_funcs = {
	.reset = drm_atomic_helper_connector_reset,
	.fill_modes = drm_helper_probe_single_connector_modes,
	.destroy = drm_connector_cleanup,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

static const struct drm_mode_config_funcs fo_mode_config_funcs = {
	.fb_create = drm_gem_fb_create,
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
};

static const struct drm_display_mode fo_display_mode = {
	DRM_MODE_INIT(60, 256, 144, 53, 30),
};

static const struct spi_device_id fo_ids[] = {
	{"flipper-one-display", (kernel_ulong_t)&fo_display_mode},
	{},
};
MODULE_DEVICE_TABLE(spi, fo_ids);

static const struct of_device_id fo_of_match[] = {
	{.compatible = "flipper,one-display", &fo_display_mode},
	{},
};
MODULE_DEVICE_TABLE(of, fo_of_match);

static const u32 fo_formats[] = {
	DRM_FORMAT_XRGB8888,
};

static int fo_pipe_init(struct drm_device *dev, struct fo_device *fo,
			const u32 *formats, unsigned int format_count,
			const u64 *format_modifiers)
{
	int ret;
	struct drm_encoder *encoder = &fo->encoder;
	struct drm_plane *plane = &fo->plane;
	struct drm_crtc *crtc = &fo->crtc;
	struct drm_connector *connector = &fo->connector;

	drm_plane_helper_add(plane, &fo_plane_helper_funcs);
	ret = drm_universal_plane_init(dev, plane, 0, &fo_plane_funcs,
				       formats, format_count,
				       format_modifiers,
				       DRM_PLANE_TYPE_PRIMARY, NULL);
	if (ret)
		return ret;

	drm_crtc_helper_add(crtc, &fo_crtc_helper_funcs);
	ret = drm_crtc_init_with_planes(dev, crtc, plane, NULL,
					&fo_crtc_funcs, NULL);
	if (ret)
		return ret;

	encoder->possible_crtcs = drm_crtc_mask(crtc);
	ret = drm_encoder_init(dev, encoder, &fo_encoder_funcs,
			       DRM_MODE_ENCODER_NONE, NULL);
	if (ret)
		return ret;

	ret = drm_connector_init(&fo->drm, &fo->connector,
				 &fo_connector_funcs,
				 DRM_MODE_CONNECTOR_SPI);
	if (ret)
		return ret;

	drm_connector_helper_add(&fo->connector, &fo_connector_hfuncs);

	return drm_connector_attach_encoder(connector, encoder);
}

static int fo_probe(struct spi_device *spi)
{
	int ret;
	struct device *dev;
	struct fo_device *fo;
	struct drm_device *drm;

	dev = &spi->dev;
	spi->bits_per_word = 16;
	spi->mode |= SPI_MODE_3;

	ret = spi_setup(spi);
	if (ret < 0)
		return dev_err_probe(dev, ret, "Failed to setup spi device\n");

	if (!dev->coherent_dma_mask) {
		ret = dma_coerce_mask_and_coherent(dev, DMA_BIT_MASK(32));
		if (ret)
			return dev_err_probe(dev, ret, "Failed to set dma mask\n");
	}

	fo = devm_drm_dev_alloc(dev, &fo_drm_driver, struct fo_device, drm);
	if (!fo)
		return -ENOMEM;

	spi_set_drvdata(spi, fo);

	fo->spi = spi;
	drm = &fo->drm;
	ret = drmm_mode_config_init(drm);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to initialize drm config\n");

	fo->active_gpio = devm_gpiod_get_optional(dev, "active", GPIOD_OUT_LOW);
	if (!fo->active_gpio)
		dev_warn(dev, "Active gpio not defined\n");

	drm->mode_config.funcs = &fo_mode_config_funcs;
	fo->mode = spi_get_device_match_data(spi);
	/* The controller expects 3-byte aligned input lines */
	fo->pitch = roundup(fo->mode->hdisplay, 3);
	fo->tx_buffer_size = (fo->pitch) * (fo->mode->vdisplay);

	fo->tx_buffer = devm_kzalloc(dev, fo->tx_buffer_size, GFP_KERNEL);
	if (!fo->tx_buffer)
		return -ENOMEM;

	drm->mode_config.min_width = fo->mode->hdisplay;
	drm->mode_config.max_width = fo->mode->hdisplay;
	drm->mode_config.min_height = fo->mode->vdisplay;
	drm->mode_config.max_height = fo->mode->vdisplay;

	ret = fo_pipe_init(drm, fo, fo_formats, ARRAY_SIZE(fo_formats), NULL);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to initialize display pipeline.\n");

	drm_mode_config_reset(drm);

	ret = drm_dev_register(drm, 0);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to register drm device.\n");

	drm_client_setup(drm, NULL);

	return 0;
}

static void fo_remove(struct spi_device *spi)
{
	struct fo_device *fo = spi_get_drvdata(spi);

	drm_dev_unplug(&fo->drm);
	drm_atomic_helper_shutdown(&fo->drm);
}

static struct spi_driver fo_spi_driver = {
	.driver = {
		.name = "flipper_one_display",
		.of_match_table = fo_of_match,
	},
	.probe = fo_probe,
	.remove = fo_remove,
	.id_table = fo_ids,
};
module_spi_driver(fo_spi_driver);

MODULE_AUTHOR("Alexey Charkov <alchark@flipper.net>");
MODULE_DESCRIPTION("SPI Protocol driver for the Flipper One display");
MODULE_LICENSE("GPL");
