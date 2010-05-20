/*-
 * Copyright (c) 2007-2010 Redpill Linpro AS
 * All rights reserved.
 *
 * Author: Dag-Erling Smørgav <des@des.no>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include "config.h"

#include "svnid.h"
SVNID("$Id$")

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "cache.h"
#include "stevedore.h"
#include "cli_priv.h"

static VTAILQ_HEAD(, stevedore)	stevedores =
    VTAILQ_HEAD_INITIALIZER(stevedores);

static const struct stevedore * volatile stv_next;

/*********************************************************************
 * NB! Dirty trick alert:
 *
 * We use a captive objcore as tail senteniel for LRU lists, but to
 * make sure it does not get into play by accident, we do _not_
 * initialize its magic with OBJCORE_MAGIC.
 *
 */

struct lru *
LRU_Alloc(void)
{
	struct lru *l;

	ALLOC_OBJ(l, LRU_MAGIC);
	AN(l);
	VLIST_INIT(&l->lru_head);
	VLIST_INSERT_HEAD(&l->lru_head, &l->senteniel, lru_list);
	return (l);
}

/*********************************************************************
 * XXX: trust pointer writes to be atomic
 */

static struct stevedore *
stv_pick_stevedore(void)
{
	struct stevedore *stv;

	/* pick a stevedore and bump the head along */
	stv = VTAILQ_NEXT(stv_next, list);
	if (stv == NULL)
		stv = VTAILQ_FIRST(&stevedores);
	AN(stv);
	AN(stv->name);
	stv_next = stv;
	return (stv);
}


/*********************************************************************/

static void
STV_InitObj(struct sess *sp, struct object *o, unsigned wsl, unsigned lhttp,
    unsigned nhttp)
{

	memset(o, 0, sizeof *o);
	o->magic = OBJECT_MAGIC;

	assert(PAOK(wsl));
	assert(PAOK(lhttp));

	o->http = HTTP_create(o + 1, nhttp);
	WS_Init(o->ws_o, "obj", (char *)(o + 1) + lhttp, wsl);
	WS_Assert(o->ws_o);

	http_Setup(o->http, o->ws_o);
	o->http->magic = HTTP_MAGIC;
	o->grace = NAN;
	o->entered = NAN;
	VTAILQ_INIT(&o->store);
	sp->wrk->stats.n_object++;
}

/*********************************************************************/

struct object *
STV_NewObject(struct sess *sp, unsigned l, double ttl, unsigned nhttp)
{
	struct object *o;
	struct storage *st;
	unsigned lh;

	(void)ttl;
	assert(l > 0);
	l = PRNDUP(l);

	lh = HTTP_estimate(nhttp);
	lh = PRNDUP(lh);

	if (!sp->wrk->cacheable) {
		o = malloc(sizeof *o + l + lh);
		XXXAN(o);
		STV_InitObj(sp, o, l, lh, nhttp);
		return (o);
	}
	st = STV_alloc(sp, sizeof *o + l + lh, sp->objcore);
	XXXAN(st);
	xxxassert(st->space >= (sizeof *o + l + lh));

	st->len = st->space;

	o = (void *)st->ptr; /* XXX: align ? */

	l = PRNDDN(st->space - (sizeof *o + lh));


	STV_InitObj(sp, o, l, lh, nhttp);
	o->objstore = st;
	return (o);
}

/*********************************************************************/

struct storage *
STV_alloc(struct sess *sp, size_t size, struct objcore *oc)
{
	struct storage *st;
	struct stevedore *stv = NULL;
	unsigned fail = 0;

	/*
	 * Always try the stevedore which allocated the object in order to
	 * not needlessly split an object across multiple stevedores.
	 */
	if (sp->obj != NULL) {
		CHECK_OBJ_NOTNULL(sp->obj, OBJECT_MAGIC);
		if (sp->obj->objstore != NULL) {
			stv = sp->obj->objstore->stevedore;
			CHECK_OBJ_NOTNULL(stv, STEVEDORE_MAGIC);
		}
	}

	for (;;) {
		if (stv == NULL) {
			stv = stv_pick_stevedore();
			fail = 0;
		}

		/* try to allocate from it */
		AN(stv->alloc);
		st = stv->alloc(stv, size, oc);
		if (st != NULL)
			break;

		/* no luck; try to free some space and keep trying */
		if (EXP_NukeOne(sp, stv->lru) == -1)
			break;

		/* Enough is enough: try another if we have one */
		if (++fail == 50)
			stv = NULL;
	}
	CHECK_OBJ_NOTNULL(st, STORAGE_MAGIC);
	return (st);
}

void
STV_trim(struct storage *st, size_t size)
{

	CHECK_OBJ_NOTNULL(st, STORAGE_MAGIC);
	AN(st->stevedore);
	if (st->stevedore->trim)
		st->stevedore->trim(st, size);
}

void
STV_free(struct storage *st)
{

	CHECK_OBJ_NOTNULL(st, STORAGE_MAGIC);
	AN(st->stevedore);
	AN(st->stevedore->free);
	st->stevedore->free(st);
}

void
STV_ready(void)
{
	struct stevedore *stv;

	ASSERT_MGT();
	VTAILQ_FOREACH(stv, &stevedores, list) {
		if (stv->ready != NULL)
			stv->ready(stv);
	}
}

void
STV_open(void)
{
	struct stevedore *stv;

	VTAILQ_FOREACH(stv, &stevedores, list) {
		if (stv->open != NULL)
			stv->open(stv);
	}
}

void
STV_close(void)
{
	struct stevedore *stv;

	VTAILQ_FOREACH(stv, &stevedores, list) {
		if (stv->close != NULL)
			stv->close(stv);
	}
}

struct lru *
STV_lru(const struct storage *st)
{
	CHECK_OBJ_NOTNULL(st, STORAGE_MAGIC);

	return (st->stevedore->lru);
}

static const struct choice STV_choice[] = {
	{ "file",	&smf_stevedore },
	{ "malloc",	&sma_stevedore },
	{ "persistent",	&smp_stevedore },
#ifdef HAVE_LIBUMEM
	{ "umem",	&smu_stevedore },
#endif
	{ NULL,		NULL }
};

/*--------------------------------------------------------------------*/

void
STV_config(const char *spec)
{
	char **av;
	const char *p, *q;
	struct stevedore *stv;
	const struct stevedore *stv2;
	int ac, l;
	static unsigned seq = 0;

	p = strchr(spec, '=');
	q = strchr(spec, ',');
	if (p != NULL && (q == NULL || q > p)) {
		av = ParseArgv(p + 1, ARGV_COMMA);
	} else {
		av = ParseArgv(spec, ARGV_COMMA);
		p = NULL;
	}
	AN(av);

	if (av[0] != NULL)
		ARGV_ERR("%s\n", av[0]);

	if (av[1] == NULL)
		ARGV_ERR("-s argument is empty\n");

	for (ac = 0; av[ac + 2] != NULL; ac++)
		continue;

	stv2 = pick(STV_choice, av[1], "storage");
	AN(stv2);

	/* Append to ident string */
	vsb_printf(vident, ",-s%s", av[1]);

	av += 2;

	CHECK_OBJ_NOTNULL(stv2, STEVEDORE_MAGIC);
	ALLOC_OBJ(stv, STEVEDORE_MAGIC);
	AN(stv);

	*stv = *stv2;
	AN(stv->name);
	AN(stv->alloc);

	if (p == NULL)
		bprintf(stv->ident, "storage_%u", seq++);
	else {
		l = p - spec;
		if (l > sizeof stv->ident - 1)
			l = sizeof stv->ident - 1;
		bprintf(stv->ident, "%*.*s", l, l, spec);
	}

	stv->lru = LRU_Alloc();

	if (stv->init != NULL)
		stv->init(stv, ac, av);
	else if (ac != 0)
		ARGV_ERR("(-s%s) too many arguments\n", stv->name);

	VTAILQ_INSERT_TAIL(&stevedores, stv, list);

	if (!stv_next)
		stv_next = VTAILQ_FIRST(&stevedores);
}

/*--------------------------------------------------------------------*/

static void
stv_cli_list(struct cli *cli, const char * const *av, void *priv)
{
	struct stevedore *stv;

	ASSERT_MGT();
	(void)av;
	(void)priv;
	cli_out(cli, "Storage devices:\n");
	VTAILQ_FOREACH(stv, &stevedores, list) {
		cli_out(cli, "\tstorage.%s.%s\n", stv->name, stv->ident);
	}
}

/*--------------------------------------------------------------------*/

struct cli_proto cli_stv[] = {
	{ "storage.list", "storage.list", "List storage devices\n",
	    0, 0, "", stv_cli_list },
	{ NULL}
};

