// SPDX-License-Identifier: GPL-2.0-or-later

/*
 * zcomp backend using the kernel crypto acompress API.
 *
 * Allows zram to offload (de)compression to hardware accelerators such as
 * Intel QAT when a matching crypto acompress driver is present.  The zcomp
 * user-visible algorithm name is "deflate-hw"; the underlying crypto
 * transform is "deflate".
 */

#include <linux/crypto.h>
#include <linux/scatterlist.h>
#include <crypto/acompress.h>
#include <crypto/scatterwalk.h>

#include "backend_acomp.h"

struct acomp_drv {
	char crypto_name[CRYPTO_MAX_ALG_NAME];
	bool async_decomp;
};

struct acomp_stream {
	struct crypto_acomp *acomp;
	struct acomp_req *req;
	struct crypto_wait wait;
};

static struct acomp_drv *acomp_drv(struct zcomp_params *params)
{
	return params->drv_data;
}

static struct acomp_stream *acomp_stream(struct zcomp_ctx *ctx)
{
	return ctx->context;
}

const char *zcomp_acomp_crypto_name(struct zcomp *comp)
{
	struct acomp_drv *drv;

	if (!comp || comp->ops != &backend_acomp_deflate)
		return NULL;

	drv = acomp_drv(comp->params);
	return drv ? drv->crypto_name : NULL;
}

bool zcomp_acomp_async_decompress(struct zcomp *comp)
{
	struct acomp_drv *drv;

	if (!comp || comp->ops != &backend_acomp_deflate)
		return false;

	drv = acomp_drv(comp->params);
	return drv && drv->async_decomp;
}

static void acomp_release_params(struct zcomp_params *params)
{
	kfree(params->drv_data);
	params->drv_data = NULL;
}

static int acomp_setup_params(struct zcomp_params *params)
{
	struct acomp_drv *drv;

	drv = kzalloc_obj(*drv);
	if (!drv)
		return -ENOMEM;

	strscpy(drv->crypto_name, "deflate", sizeof(drv->crypto_name));
	params->drv_data = drv;
	return 0;
}

static void acomp_destroy_ctx(struct zcomp_ctx *ctx)
{
	struct acomp_stream *stream = acomp_stream(ctx);

	if (!stream)
		return;

	crypto_free_acomp(stream->acomp);
	acomp_request_free(stream->req);
	kfree(stream);
	ctx->context = NULL;
}

static int acomp_create_ctx(struct zcomp_params *params, struct zcomp_ctx *ctx)
{
	struct acomp_drv *drv = acomp_drv(params);
	struct acomp_stream *stream;
	int ret;

	stream = kzalloc_obj(*stream);
	if (!stream)
		return -ENOMEM;

	/*
	 * Prefer an async driver (e.g. qat_deflate) when present; fall back to
	 * the synchronous generic implementation otherwise.
	 */
	stream->acomp = crypto_alloc_acomp(drv->crypto_name, 0, CRYPTO_ALG_ASYNC);
	if (IS_ERR(stream->acomp)) {
		ret = PTR_ERR(stream->acomp);
		goto err_free;
	}

	stream->req = acomp_request_alloc(stream->acomp);
	if (!stream->req) {
		ret = -ENOMEM;
		goto err_free_acomp;
	}

	crypto_init_wait(&stream->wait);
	acomp_request_set_callback(stream->req, CRYPTO_TFM_REQ_MAY_BACKLOG,
				   crypto_req_done, &stream->wait);

	if (!drv->async_decomp && acomp_is_async(stream->acomp))
		drv->async_decomp = true;

	ctx->context = stream;
	return 0;

err_free_acomp:
	crypto_free_acomp(stream->acomp);
err_free:
	kfree(stream);
	return ret;
}

static int acomp_do(struct zcomp_params *params, struct zcomp_ctx *ctx,
		    struct zcomp_req *req, bool comp)
{
	struct acomp_stream *stream = acomp_stream(ctx);
	struct scatterlist src, dst;
	int ret;

	sg_init_one(&src, (void *)req->src, req->src_len);
	sg_init_one(&dst, req->dst, req->dst_len);
	acomp_request_set_params(stream->req, &src, &dst, req->src_len,
				 req->dst_len);

	if (comp)
		ret = crypto_wait_req(crypto_acomp_compress(stream->req),
				      &stream->wait);
	else
		ret = crypto_wait_req(crypto_acomp_decompress(stream->req),
				      &stream->wait);

	if (!ret)
		req->dst_len = stream->req->dlen;
	return ret;
}

static int acomp_compress(struct zcomp_params *params, struct zcomp_ctx *ctx,
			    struct zcomp_req *req)
{
	return acomp_do(params, ctx, req, true);
}

static int acomp_decompress(struct zcomp_params *params, struct zcomp_ctx *ctx,
			      struct zcomp_req *req)
{
	return acomp_do(params, ctx, req, false);
}

int zcomp_acomp_decompress_async(struct zcomp *comp, struct zcomp_strm *zstrm,
				 const void *src, unsigned int src_len,
				 struct page *dst, crypto_completion_t cb,
				 void *data)
{
	struct acomp_stream *stream = acomp_stream(&zstrm->ctx);
	struct scatterlist ssg, dsg;

	if (!zcomp_acomp_async_decompress(comp))
		return -EOPNOTSUPP;

	sg_init_one(&ssg, (void *)src, src_len);
	sg_init_table(&dsg, 1);
	sg_set_page(&dsg, dst, PAGE_SIZE, 0);
	acomp_request_set_params(stream->req, &ssg, &dsg, src_len, PAGE_SIZE);
	acomp_request_set_callback(stream->req, CRYPTO_TFM_REQ_MAY_BACKLOG, cb,
				   data);

	return crypto_acomp_decompress(stream->req);
}

const struct zcomp_ops backend_acomp_deflate = {
	.compress	= acomp_compress,
	.decompress	= acomp_decompress,
	.create_ctx	= acomp_create_ctx,
	.destroy_ctx	= acomp_destroy_ctx,
	.setup_params	= acomp_setup_params,
	.release_params	= acomp_release_params,
	.name		= "deflate-hw",
};
