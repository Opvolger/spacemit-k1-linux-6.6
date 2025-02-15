// SPDX-License-Identifier: GPL-2.0
/*
 * Support for Spacemit k1x spi controller dma mode
 *
 * Copyright (c) 2023, spacemit Corporation.
 *
 */

#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/dmaengine.h>
#include <linux/scatterlist.h>
#include <linux/sizes.h>
#include <linux/spi/spi.h>
#include <linux/delay.h>

#include "spi-k1x.h"

static int k1x_spi_map_dma_buffer(struct spi_driver_data *drv_data,
				enum dma_data_direction dir)
{
	int i, nents, len = drv_data->len;
	struct scatterlist *sg;
	struct device *dmadev;
	struct sg_table *sgt;
	void *buf, *pbuf;

	if (dir == DMA_TO_DEVICE) {
		dmadev = drv_data->tx_chan->device->dev;
		sgt = &drv_data->tx_sgt;
		buf = drv_data->tx;
		drv_data->tx_map_len = len;
	} else {
		dmadev = drv_data->rx_chan->device->dev;
		sgt = &drv_data->rx_sgt;
		buf = drv_data->rx;
		drv_data->rx_map_len = len;
	}

	nents = DIV_ROUND_UP(len, SZ_2K);
	if (nents != sgt->nents) {
		int ret;

		sg_free_table(sgt);
		ret = sg_alloc_table(sgt, nents, GFP_ATOMIC);
		if (ret)
			return ret;
	}

	pbuf = buf;
	for_each_sg(sgt->sgl, sg, sgt->nents, i) {
		size_t bytes = min_t(size_t, len, SZ_2K);

		if (buf)
			sg_set_buf(sg, pbuf, bytes);
		else
			sg_set_buf(sg, drv_data->dummy, bytes);

		pbuf += bytes;
		len -= bytes;
	}

	nents = dma_map_sg(dmadev, sgt->sgl, sgt->nents, dir);
	if (!nents)
		return -ENOMEM;

	return nents;
}

static void k1x_spi_unmap_dma_buffer(struct spi_driver_data *drv_data,
					enum dma_data_direction dir)
{
	struct device *dmadev;
	struct sg_table *sgt;

	if (dir == DMA_TO_DEVICE) {
		dmadev = drv_data->tx_chan->device->dev;
		sgt = &drv_data->tx_sgt;
	} else {
		dmadev = drv_data->rx_chan->device->dev;
		sgt = &drv_data->rx_sgt;
	}

	dma_unmap_sg(dmadev, sgt->sgl, sgt->nents, dir);
}

static void k1x_spi_unmap_dma_buffers(struct spi_driver_data *drv_data)
{
	if (!drv_data->dma_mapped)
		return;

	k1x_spi_unmap_dma_buffer(drv_data, DMA_FROM_DEVICE);
	k1x_spi_unmap_dma_buffer(drv_data, DMA_TO_DEVICE);

	drv_data->dma_mapped = 0;
}

static void k1x_spi_dma_transfer_complete(struct spi_driver_data *drv_data,
					     bool error)
{
	struct spi_message *msg = drv_data->cur_msg;

	/*
	 * It is possible that one CPU is handling ROR interrupt and other
	 * just gets DMA completion. Calling pump_transfers() twice for the
	 * same transfer leads to problems thus we prevent concurrent calls
	 * by using ->dma_running.
	 */
	if (atomic_dec_and_test(&drv_data->dma_running)) {
		/*
		 * If the other CPU is still handling the ROR interrupt we
		 * might not know about the error yet. So we re-check the
		 * ROR bit here before we clear the status register.
		 */
		if (!error) {
			u32 status = k1x_spi_read(drv_data, STATUS)
				     & drv_data->mask_sr;
			error = status & STATUS_ROR;
		}

		/* Clear status & disable interrupts */
		k1x_spi_write(drv_data, FIFO_CTRL,
				k1x_spi_read(drv_data, FIFO_CTRL)
				& ~drv_data->dma_fifo_ctrl);
		k1x_spi_write(drv_data, TOP_CTRL,
				k1x_spi_read(drv_data, TOP_CTRL)
				& ~drv_data->dma_top_ctrl);
		k1x_spi_write(drv_data, STATUS, drv_data->clear_sr);
		k1x_spi_write(drv_data, TO, 0);

		if (!error) {
			k1x_spi_unmap_dma_buffers(drv_data);

			drv_data->tx += drv_data->tx_map_len;
			drv_data->rx += drv_data->rx_map_len;

			msg->actual_length += drv_data->len;
			msg->state = k1x_spi_next_transfer(drv_data);
		} else {
			/* In case we got an error we disable the SSP now */
			k1x_spi_write(drv_data, TOP_CTRL,
					 k1x_spi_read(drv_data, TOP_CTRL)
					 & ~TOP_SSE);

			msg->state = ERROR_STATE;
		}
		queue_work(system_wq, &drv_data->pump_transfers);
	}
}

static void k1x_spi_dma_callback(void *data)
{
	k1x_spi_dma_transfer_complete(data, false);
}

static struct dma_async_tx_descriptor *
k1x_spi_dma_prepare_one(struct spi_driver_data *drv_data,
			   enum dma_transfer_direction dir)
{
	struct chip_data *chip = drv_data->cur_chip;
	enum dma_slave_buswidth width;
	struct dma_slave_config cfg;
	struct dma_chan *chan;
	struct sg_table *sgt;
	int nents, ret;

	switch (drv_data->n_bytes) {
	case 1:
		width = DMA_SLAVE_BUSWIDTH_1_BYTE;
		break;
	case 2:
		width = DMA_SLAVE_BUSWIDTH_2_BYTES;
		break;
	default:
		width = DMA_SLAVE_BUSWIDTH_4_BYTES;
		break;
	}

	memset(&cfg, 0, sizeof(cfg));
	cfg.direction = dir;

	if (dir == DMA_MEM_TO_DEV) {
		cfg.dst_addr = drv_data->ssdr_physical;
		cfg.dst_addr_width = width;
		cfg.dst_maxburst = chip->dma_burst_size;

		sgt = &drv_data->tx_sgt;
		nents = drv_data->tx_nents;
		chan = drv_data->tx_chan;
	} else {
		cfg.src_addr = drv_data->ssdr_physical;
		cfg.src_addr_width = width;
		cfg.src_maxburst = chip->dma_burst_size;

		sgt = &drv_data->rx_sgt;
		nents = drv_data->rx_nents;
		chan = drv_data->rx_chan;
	}

	ret = dmaengine_slave_config(chan, &cfg);
	if (ret) {
		dev_warn(&drv_data->pdev->dev, "DMA slave config failed\n");
		return NULL;
	}

	return dmaengine_prep_slave_sg(chan, sgt->sgl, nents, dir,
			DMA_PREP_INTERRUPT | DMA_CTRL_ACK);
}

bool k1x_spi_dma_is_possible(size_t len)
{
	return len <= MAX_DMA_LEN;
}

int k1x_spi_map_dma_buffers(struct spi_driver_data *drv_data)
{
	const struct chip_data *chip = drv_data->cur_chip;
	int ret;

	if (!chip->enable_dma)
		return 0;

	/* Don't bother with DMA if we can't do even a single burst */
	if (drv_data->len < chip->dma_burst_size)
		return 0;

	ret = k1x_spi_map_dma_buffer(drv_data, DMA_TO_DEVICE);
	if (ret <= 0) {
		dev_warn(&drv_data->pdev->dev, "failed to DMA map TX\n");
		return 0;
	}

	drv_data->tx_nents = ret;

	ret = k1x_spi_map_dma_buffer(drv_data, DMA_FROM_DEVICE);
	if (ret <= 0) {
		k1x_spi_unmap_dma_buffer(drv_data, DMA_TO_DEVICE);
		dev_warn(&drv_data->pdev->dev, "failed to DMA map RX\n");
		return 0;
	}

	drv_data->rx_nents = ret;
	return 1;
}

irqreturn_t k1x_spi_dma_transfer(struct spi_driver_data *drv_data)
{
	u32 status;

	status = k1x_spi_read(drv_data, STATUS) & drv_data->mask_sr;

	if (((drv_data->slave_mode) && (status & STATUS_TINT)) ||
			(status & STATUS_ROR)) {
		dmaengine_terminate_all(drv_data->rx_chan);
		dmaengine_terminate_all(drv_data->tx_chan);
		k1x_spi_dma_transfer_complete(drv_data, true);
		return IRQ_HANDLED;
	}

	return IRQ_NONE;
}

void k1x_spi_slave_sw_timeout_callback(struct spi_driver_data *drv_data)
{
	dmaengine_terminate_all(drv_data->rx_chan);
	dmaengine_terminate_all(drv_data->tx_chan);
	k1x_spi_dma_transfer_complete(drv_data, true);
}

int k1x_spi_dma_prepare(struct spi_driver_data *drv_data, u32 dma_burst)
{
	struct dma_async_tx_descriptor *tx_desc, *rx_desc;

	tx_desc = k1x_spi_dma_prepare_one(drv_data, DMA_MEM_TO_DEV);
	if (!tx_desc) {
		dev_err(&drv_data->pdev->dev,
			"failed to get DMA TX descriptor\n");
		return -EBUSY;
	}

	rx_desc = k1x_spi_dma_prepare_one(drv_data, DMA_DEV_TO_MEM);
	if (!rx_desc) {
		dev_err(&drv_data->pdev->dev,
			"failed to get DMA RX descriptor\n");
		return -EBUSY;
	}

	/* We are ready when RX completes */
	rx_desc->callback = k1x_spi_dma_callback;
	rx_desc->callback_param = drv_data;

	dmaengine_submit(rx_desc);
	dmaengine_submit(tx_desc);
	return 0;
}

void k1x_spi_dma_start(struct spi_driver_data *drv_data)
{
	dma_async_issue_pending(drv_data->rx_chan);
	dma_async_issue_pending(drv_data->tx_chan);

	atomic_set(&drv_data->dma_running, 1);
}

int k1x_spi_dma_setup(struct spi_driver_data *drv_data)
{
	struct k1x_spi_master *pdata = drv_data->master_info;
	struct device *dev = &drv_data->pdev->dev;
	dma_cap_mask_t mask;

	dma_cap_zero(mask);
	dma_cap_set(DMA_SLAVE, mask);

	drv_data->dummy = devm_kzalloc(dev, SZ_2K, GFP_KERNEL);
	if (!drv_data->dummy)
		return -ENOMEM;

	drv_data->tx_chan = dma_request_slave_channel_compat(mask,
				pdata->dma_filter, pdata->tx_param, dev, "tx");
	if (!drv_data->tx_chan)
		return -ENODEV;

	drv_data->rx_chan = dma_request_slave_channel_compat(mask,
				pdata->dma_filter, pdata->rx_param, dev, "rx");
	if (!drv_data->rx_chan) {
		dma_release_channel(drv_data->tx_chan);
		drv_data->tx_chan = NULL;
		return -ENODEV;
	}

	return 0;
}

void k1x_spi_dma_release(struct spi_driver_data *drv_data)
{
	if (drv_data->rx_chan) {
		dmaengine_terminate_all(drv_data->rx_chan);
		dma_release_channel(drv_data->rx_chan);
		sg_free_table(&drv_data->rx_sgt);
		drv_data->rx_chan = NULL;
	}
	if (drv_data->tx_chan) {
		dmaengine_terminate_all(drv_data->tx_chan);
		dma_release_channel(drv_data->tx_chan);
		sg_free_table(&drv_data->tx_sgt);
		drv_data->tx_chan = NULL;
	}
}

int k1x_spi_set_dma_burst_and_threshold(struct chip_data *chip,
					   struct spi_device *spi,
					   u8 bits_per_word, u32 *burst_code,
					   u32 *threshold)
{
	/*
	 * If the DMA burst size is given in chip_info we use
	 * that, otherwise we set it to half of FIFO size; SPI
	 * FIFO has 16 entry, so FIFO size = 16*bits_per_word/8;
	 * Also we use the default FIFO thresholds for now.
	 */
	if (chip && chip->dma_burst_size)
		*burst_code = chip->dma_burst_size;
	else if (bits_per_word <= 8) {
		*burst_code = 8;
	}
	else if (bits_per_word <= 16)
		*burst_code = 16;
	else
		*burst_code = 32;

	*threshold = FIFO_RxTresh(RX_THRESH_DFLT)
		   | FIFO_TxTresh(TX_THRESH_DFLT);

	return 0;
}
