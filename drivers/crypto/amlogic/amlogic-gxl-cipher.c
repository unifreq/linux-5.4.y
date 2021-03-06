// SPDX-License-Identifier: GPL-2.0
/*
 * amlogic-cipher.c - hardware cryptographic offloader for Amlogic GXL SoC
 *
 * Copyright (C) 2018-2019 Corentin LABBE <clabbe@baylibre.com>
 *
 * This file add support for AES cipher with 128,192,256 bits keysize in
 * CBC and ECB mode.
 */

#include <linux/crypto.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <crypto/scatterwalk.h>
#include <linux/scatterlist.h>
#include <linux/dma-mapping.h>
#include <crypto/internal/skcipher.h>
#include "amlogic-gxl.h"

static int get_engine_number(struct meson_dev *mc)
{
	return atomic_inc_return(&mc->flow) % MAXFLOW;
}

static bool meson_cipher_need_fallback(struct skcipher_request *areq)
{
	struct scatterlist *src_sg = areq->src;
	struct scatterlist *dst_sg = areq->dst;

	if (areq->cryptlen == 0)
		return true;

	if (sg_nents(src_sg) != sg_nents(dst_sg))
		return true;

	/* KEY/IV descriptors use 3 desc */
	if (sg_nents(src_sg) > MAXDESC - 3 || sg_nents(dst_sg) > MAXDESC - 3)
		return true;

	while (src_sg && dst_sg) {
		if ((src_sg->length % 16) != 0)
			return true;
		if ((dst_sg->length % 16) != 0)
			return true;
		if (src_sg->length != dst_sg->length)
			return true;
		if (!IS_ALIGNED(src_sg->offset, sizeof(u32)))
			return true;
		if (!IS_ALIGNED(dst_sg->offset, sizeof(u32)))
			return true;
		src_sg = sg_next(src_sg);
		dst_sg = sg_next(dst_sg);
	}

	return false;
}

static int meson_cipher_do_fallback(struct skcipher_request *areq)
{
	struct crypto_skcipher *tfm = crypto_skcipher_reqtfm(areq);
	struct meson_cipher_tfm_ctx *op = crypto_skcipher_ctx(tfm);
	struct meson_cipher_req_ctx *rctx = skcipher_request_ctx(areq);
	int err;
#ifdef CONFIG_CRYPTO_DEV_AMLOGIC_GXL_DEBUG
	struct skcipher_alg *alg = crypto_skcipher_alg(tfm);
	struct meson_alg_template *algt;
#endif
	SYNC_SKCIPHER_REQUEST_ON_STACK(req, op->fallback_tfm);

#ifdef CONFIG_CRYPTO_DEV_AMLOGIC_GXL_DEBUG
	algt = container_of(alg, struct meson_alg_template, alg.skcipher);
	algt->stat_fb++;
#endif
	skcipher_request_set_sync_tfm(req, op->fallback_tfm);
	skcipher_request_set_callback(req, areq->base.flags, NULL, NULL);
	skcipher_request_set_crypt(req, areq->src, areq->dst,
				   areq->cryptlen, areq->iv);
	if (rctx->op_dir == MESON_DECRYPT)
		err = crypto_skcipher_decrypt(req);
	else
		err = crypto_skcipher_encrypt(req);
	skcipher_request_zero(req);
	return err;
}

static int meson_cipher(struct skcipher_request *areq)
{
	struct crypto_skcipher *tfm = crypto_skcipher_reqtfm(areq);
	struct meson_cipher_tfm_ctx *op = crypto_skcipher_ctx(tfm);
	struct meson_cipher_req_ctx *rctx = skcipher_request_ctx(areq);
	struct meson_dev *mc = op->mc;
	struct skcipher_alg *alg = crypto_skcipher_alg(tfm);
	struct meson_alg_template *algt;
	int flow = rctx->flow;
	unsigned int todo, eat, len;
	struct scatterlist *src_sg = areq->src;
	struct scatterlist *dst_sg = areq->dst;
	struct meson_desc *desc;
	int nr_sgs, nr_sgd;
	int i, err = 0;
	unsigned int keyivlen, ivsize, offset, tloffset;
	dma_addr_t phykeyiv;
	void *backup_iv = NULL, *bkeyiv;

	algt = container_of(alg, struct meson_alg_template, alg.skcipher);

	dev_dbg(mc->dev, "%s %s %u %x IV(%u) key=%u flow=%d\n", __func__,
		crypto_tfm_alg_name(areq->base.tfm),
		areq->cryptlen,
		rctx->op_dir, crypto_skcipher_ivsize(tfm),
		op->keylen, flow);

#ifdef CONFIG_CRYPTO_DEV_AMLOGIC_GXL_DEBUG
	algt->stat_req++;
	mc->chanlist[flow].stat_req++;
#endif

	/*
	 * The hardware expect a list of meson_desc structures.
	 * The 2 first structures store key
	 * The third stores IV
	 */
	bkeyiv = kzalloc(48, GFP_KERNEL | GFP_DMA);
	if (!bkeyiv)
		return -ENOMEM;

	memcpy(bkeyiv, op->key, op->keylen);
	keyivlen = op->keylen;

	ivsize = crypto_skcipher_ivsize(tfm);
	if (areq->iv && ivsize > 0) {
		if (ivsize > areq->cryptlen) {
			dev_err(mc->dev, "invalid ivsize=%d vs len=%d\n", ivsize, areq->cryptlen);
			return -EINVAL;
		}
		memcpy(bkeyiv + 32, areq->iv, ivsize);
		keyivlen = 48;
		if (rctx->op_dir == MESON_DECRYPT) {
			backup_iv = kzalloc(ivsize, GFP_KERNEL);
			if (!backup_iv) {
				err = -ENOMEM;
				goto theend;
			}
			offset = areq->cryptlen - ivsize;
			scatterwalk_map_and_copy(backup_iv, areq->src, offset,
						 ivsize, 0);
		}
	}
	if (keyivlen == 24)
		keyivlen = 32;

	phykeyiv = dma_map_single(mc->dev, bkeyiv, keyivlen,
				  DMA_TO_DEVICE);
	if (dma_mapping_error(mc->dev, phykeyiv)) {
		dev_err(mc->dev, "Cannot DMA MAP KEY IV\n");
		return -EFAULT;
	}

	tloffset = 0;
	eat = 0;
	i = 0;
	while (keyivlen > eat) {
		desc = &mc->chanlist[flow].tl[tloffset];
		memset(desc, 0, sizeof(struct meson_desc));
		todo = min(keyivlen - eat, 16u);
		desc->t_src = phykeyiv + i * 16;
		desc->t_dst = i * 16;
		desc->len = 16;
		desc->mode = MODE_KEY;
		desc->owner = 1;
		eat += todo;
		i++;
		tloffset++;
	}

	if (areq->src == areq->dst) {
		nr_sgs = dma_map_sg(mc->dev, areq->src, sg_nents(areq->src),
				    DMA_BIDIRECTIONAL);
		if (nr_sgs < 0) {
			dev_err(mc->dev, "Invalid SG count %d\n", nr_sgs);
			err = -EINVAL;
			goto theend;
		}
		nr_sgd = nr_sgs;
	} else {
		nr_sgs = dma_map_sg(mc->dev, areq->src, sg_nents(areq->src),
				    DMA_TO_DEVICE);
		if (nr_sgs < 0 || nr_sgs > MAXDESC - 3) {
			dev_err(mc->dev, "Invalid SG count %d\n", nr_sgs);
			err = -EINVAL;
			goto theend;
		}
		nr_sgd = dma_map_sg(mc->dev, areq->dst, sg_nents(areq->dst),
				    DMA_FROM_DEVICE);
		if (nr_sgd < 0 || nr_sgd > MAXDESC - 3) {
			dev_err(mc->dev, "Invalid SG count %d\n", nr_sgd);
			err = -EINVAL;
			goto theend;
		}
	}

	src_sg = areq->src;
	dst_sg = areq->dst;
	len = areq->cryptlen;
	while (src_sg) {
		desc = &mc->chanlist[flow].tl[tloffset];
		memset(desc, 0, sizeof(struct meson_desc));

		desc->t_src = sg_dma_address(src_sg);
		desc->t_dst = sg_dma_address(dst_sg);
		todo = min(len, sg_dma_len(src_sg));
		desc->owner = 1;
		desc->len = todo;
		desc->mode = op->keymode;
		desc->op_mode = algt->blockmode;
		desc->enc = rctx->op_dir;
		len -= todo;

		if (!sg_next(src_sg))
			desc->eoc = 1;
		tloffset++;
		src_sg = sg_next(src_sg);
		dst_sg = sg_next(dst_sg);
	}

	reinit_completion(&mc->chanlist[flow].complete);
	mc->chanlist[flow].status = 0;
	writel(mc->chanlist[flow].t_phy | 2, mc->base + (flow << 2));
	wait_for_completion_interruptible_timeout(&mc->chanlist[flow].complete,
						  msecs_to_jiffies(500));
	if (mc->chanlist[flow].status == 0) {
		dev_err(mc->dev, "DMA timeout for flow %d\n", flow);
		err = -EINVAL;
	}

	dma_unmap_single(mc->dev, phykeyiv, keyivlen, DMA_TO_DEVICE);

	if (areq->src == areq->dst) {
		dma_unmap_sg(mc->dev, areq->src, nr_sgs, DMA_BIDIRECTIONAL);
	} else {
		dma_unmap_sg(mc->dev, areq->src, nr_sgs, DMA_TO_DEVICE);
		dma_unmap_sg(mc->dev, areq->dst, nr_sgd, DMA_FROM_DEVICE);
	}

	if (areq->iv && ivsize > 0) {
		if (rctx->op_dir == MESON_DECRYPT) {
			memcpy(areq->iv, backup_iv, ivsize);
			kzfree(backup_iv);
		} else {
			scatterwalk_map_and_copy(areq->iv, areq->dst,
						 areq->cryptlen - ivsize,
						 ivsize, 0);
		}
	}
theend:
	kzfree(bkeyiv);

	return err;
}

static int meson_handle_cipher_request(struct crypto_engine *engine,
				       void *areq)
{
	int err;
	struct skcipher_request *breq = container_of(areq, struct skcipher_request, base);

	err = meson_cipher(breq);
	crypto_finalize_skcipher_request(engine, breq, err);

	return 0;
}

int meson_skdecrypt(struct skcipher_request *areq)
{
	struct crypto_skcipher *tfm = crypto_skcipher_reqtfm(areq);
	struct meson_cipher_tfm_ctx *op = crypto_skcipher_ctx(tfm);
	struct meson_cipher_req_ctx *rctx = skcipher_request_ctx(areq);
	struct crypto_engine *engine;
	int e;

	rctx->op_dir = MESON_DECRYPT;
	if (meson_cipher_need_fallback(areq))
		return meson_cipher_do_fallback(areq);
	e = get_engine_number(op->mc);
	engine = op->mc->chanlist[e].engine;
	rctx->flow = e;

	return crypto_transfer_skcipher_request_to_engine(engine, areq);
}

int meson_skencrypt(struct skcipher_request *areq)
{
	struct crypto_skcipher *tfm = crypto_skcipher_reqtfm(areq);
	struct meson_cipher_tfm_ctx *op = crypto_skcipher_ctx(tfm);
	struct meson_cipher_req_ctx *rctx = skcipher_request_ctx(areq);
	struct crypto_engine *engine;
	int e;

	rctx->op_dir = MESON_ENCRYPT;
	if (meson_cipher_need_fallback(areq))
		return meson_cipher_do_fallback(areq);
	e = get_engine_number(op->mc);
	engine = op->mc->chanlist[e].engine;
	rctx->flow = e;

	return crypto_transfer_skcipher_request_to_engine(engine, areq);
}

int meson_cipher_init(struct crypto_tfm *tfm)
{
	struct meson_cipher_tfm_ctx *op = crypto_tfm_ctx(tfm);
	struct meson_alg_template *algt;
	const char *name = crypto_tfm_alg_name(tfm);
	struct crypto_skcipher *sktfm = __crypto_skcipher_cast(tfm);
	struct skcipher_alg *alg = crypto_skcipher_alg(sktfm);

	memset(op, 0, sizeof(struct meson_cipher_tfm_ctx));

	algt = container_of(alg, struct meson_alg_template, alg.skcipher);
	op->mc = algt->mc;

	sktfm->reqsize = sizeof(struct meson_cipher_req_ctx);

	op->fallback_tfm = crypto_alloc_sync_skcipher(name, 0, CRYPTO_ALG_NEED_FALLBACK);
	if (IS_ERR(op->fallback_tfm)) {
		dev_err(op->mc->dev, "ERROR: Cannot allocate fallback for %s %ld\n",
			name, PTR_ERR(op->fallback_tfm));
		return PTR_ERR(op->fallback_tfm);
	}

	op->enginectx.op.do_one_request = meson_handle_cipher_request;
	op->enginectx.op.prepare_request = NULL;
	op->enginectx.op.unprepare_request = NULL;

	return 0;
}

void meson_cipher_exit(struct crypto_tfm *tfm)
{
	struct meson_cipher_tfm_ctx *op = crypto_tfm_ctx(tfm);

	if (op->key) {
		memzero_explicit(op->key, op->keylen);
		kfree(op->key);
	}
	crypto_free_sync_skcipher(op->fallback_tfm);
}

int meson_aes_setkey(struct crypto_skcipher *tfm, const u8 *key,
		     unsigned int keylen)
{
	struct meson_cipher_tfm_ctx *op = crypto_skcipher_ctx(tfm);
	struct meson_dev *mc = op->mc;

	switch (keylen) {
	case 128 / 8:
		op->keymode = MODE_AES_128;
		break;
	case 192 / 8:
		op->keymode = MODE_AES_192;
		break;
	case 256 / 8:
		op->keymode = MODE_AES_256;
		break;
	default:
		dev_dbg(mc->dev, "ERROR: Invalid keylen %u\n", keylen);
		crypto_skcipher_set_flags(tfm, CRYPTO_TFM_RES_BAD_KEY_LEN);
		return -EINVAL;
	}
	if (op->key) {
		memzero_explicit(op->key, op->keylen);
		kfree(op->key);
	}
	op->keylen = keylen;
	op->key = kmalloc(keylen, GFP_KERNEL | GFP_DMA);
	if (!op->key)
		return -ENOMEM;
	memcpy(op->key, key, keylen);

	return crypto_sync_skcipher_setkey(op->fallback_tfm, key, keylen);
}
