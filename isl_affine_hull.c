/*
 * Copyright 2008-2009 Katholieke Universiteit Leuven
 *
 * Use of this software is governed by the GNU LGPLv2.1 license
 *
 * Written by Sven Verdoolaege, K.U.Leuven, Departement
 * Computerwetenschappen, Celestijnenlaan 200A, B-3001 Leuven, Belgium
 */

#include "isl_ctx.h"
#include "isl_seq.h"
#include "isl_set.h"
#include "isl_lp.h"
#include "isl_map.h"
#include "isl_map_private.h"
#include "isl_equalities.h"
#include "isl_sample.h"
#include "isl_tab.h"

struct isl_basic_map *isl_basic_map_implicit_equalities(
						struct isl_basic_map *bmap)
{
	struct isl_tab *tab;

	if (!bmap)
		return bmap;

	bmap = isl_basic_map_gauss(bmap, NULL);
	if (ISL_F_ISSET(bmap, ISL_BASIC_MAP_EMPTY))
		return bmap;
	if (ISL_F_ISSET(bmap, ISL_BASIC_MAP_NO_IMPLICIT))
		return bmap;
	if (bmap->n_ineq <= 1)
		return bmap;

	tab = isl_tab_from_basic_map(bmap);
	tab = isl_tab_detect_implicit_equalities(tab);
	bmap = isl_basic_map_update_from_tab(bmap, tab);
	isl_tab_free(tab);
	bmap = isl_basic_map_gauss(bmap, NULL);
	ISL_F_SET(bmap, ISL_BASIC_MAP_NO_IMPLICIT);
	return bmap;
}

struct isl_basic_set *isl_basic_set_implicit_equalities(
						struct isl_basic_set *bset)
{
	return (struct isl_basic_set *)
		isl_basic_map_implicit_equalities((struct isl_basic_map*)bset);
}

struct isl_map *isl_map_implicit_equalities(struct isl_map *map)
{
	int i;

	if (!map)
		return map;

	for (i = 0; i < map->n; ++i) {
		map->p[i] = isl_basic_map_implicit_equalities(map->p[i]);
		if (!map->p[i])
			goto error;
	}

	return map;
error:
	isl_map_free(map);
	return NULL;
}

/* Make eq[row][col] of both bmaps equal so we can add the row
 * add the column to the common matrix.
 * Note that because of the echelon form, the columns of row row
 * after column col are zero.
 */
static void set_common_multiple(
	struct isl_basic_set *bset1, struct isl_basic_set *bset2,
	unsigned row, unsigned col)
{
	isl_int m, c;

	if (isl_int_eq(bset1->eq[row][col], bset2->eq[row][col]))
		return;

	isl_int_init(c);
	isl_int_init(m);
	isl_int_lcm(m, bset1->eq[row][col], bset2->eq[row][col]);
	isl_int_divexact(c, m, bset1->eq[row][col]);
	isl_seq_scale(bset1->eq[row], bset1->eq[row], c, col+1);
	isl_int_divexact(c, m, bset2->eq[row][col]);
	isl_seq_scale(bset2->eq[row], bset2->eq[row], c, col+1);
	isl_int_clear(c);
	isl_int_clear(m);
}

/* Delete a given equality, moving all the following equalities one up.
 */
static void delete_row(struct isl_basic_set *bset, unsigned row)
{
	isl_int *t;
	int r;

	t = bset->eq[row];
	bset->n_eq--;
	for (r = row; r < bset->n_eq; ++r)
		bset->eq[r] = bset->eq[r+1];
	bset->eq[bset->n_eq] = t;
}

/* Make first row entries in column col of bset1 identical to
 * those of bset2, using the fact that entry bset1->eq[row][col]=a
 * is non-zero.  Initially, these elements of bset1 are all zero.
 * For each row i < row, we set
 *		A[i] = a * A[i] + B[i][col] * A[row]
 *		B[i] = a * B[i]
 * so that
 *		A[i][col] = B[i][col] = a * old(B[i][col])
 */
static void construct_column(
	struct isl_basic_set *bset1, struct isl_basic_set *bset2,
	unsigned row, unsigned col)
{
	int r;
	isl_int a;
	isl_int b;
	unsigned total;

	isl_int_init(a);
	isl_int_init(b);
	total = 1 + isl_basic_set_n_dim(bset1);
	for (r = 0; r < row; ++r) {
		if (isl_int_is_zero(bset2->eq[r][col]))
			continue;
		isl_int_gcd(b, bset2->eq[r][col], bset1->eq[row][col]);
		isl_int_divexact(a, bset1->eq[row][col], b);
		isl_int_divexact(b, bset2->eq[r][col], b);
		isl_seq_combine(bset1->eq[r], a, bset1->eq[r],
					      b, bset1->eq[row], total);
		isl_seq_scale(bset2->eq[r], bset2->eq[r], a, total);
	}
	isl_int_clear(a);
	isl_int_clear(b);
	delete_row(bset1, row);
}

/* Make first row entries in column col of bset1 identical to
 * those of bset2, using only these entries of the two matrices.
 * Let t be the last row with different entries.
 * For each row i < t, we set
 *	A[i] = (A[t][col]-B[t][col]) * A[i] + (B[i][col]-A[i][col) * A[t]
 *	B[i] = (A[t][col]-B[t][col]) * B[i] + (B[i][col]-A[i][col) * B[t]
 * so that
 *	A[i][col] = B[i][col] = old(A[t][col]*B[i][col]-A[i][col]*B[t][col])
 */
static int transform_column(
	struct isl_basic_set *bset1, struct isl_basic_set *bset2,
	unsigned row, unsigned col)
{
	int i, t;
	isl_int a, b, g;
	unsigned total;

	for (t = row-1; t >= 0; --t)
		if (isl_int_ne(bset1->eq[t][col], bset2->eq[t][col]))
			break;
	if (t < 0)
		return 0;

	total = 1 + isl_basic_set_n_dim(bset1);
	isl_int_init(a);
	isl_int_init(b);
	isl_int_init(g);
	isl_int_sub(b, bset1->eq[t][col], bset2->eq[t][col]);
	for (i = 0; i < t; ++i) {
		isl_int_sub(a, bset2->eq[i][col], bset1->eq[i][col]);
		isl_int_gcd(g, a, b);
		isl_int_divexact(a, a, g);
		isl_int_divexact(g, b, g);
		isl_seq_combine(bset1->eq[i], g, bset1->eq[i], a, bset1->eq[t],
				total);
		isl_seq_combine(bset2->eq[i], g, bset2->eq[i], a, bset2->eq[t],
				total);
	}
	isl_int_clear(a);
	isl_int_clear(b);
	isl_int_clear(g);
	delete_row(bset1, t);
	delete_row(bset2, t);
	return 1;
}

/* The implementation is based on Section 5.2 of Michael Karr,
 * "Affine Relationships Among Variables of a Program",
 * except that the echelon form we use starts from the last column
 * and that we are dealing with integer coefficients.
 */
static struct isl_basic_set *affine_hull(
	struct isl_basic_set *bset1, struct isl_basic_set *bset2)
{
	unsigned total;
	int col;
	int row;

	total = 1 + isl_basic_set_n_dim(bset1);

	row = 0;
	for (col = total-1; col >= 0; --col) {
		int is_zero1 = row >= bset1->n_eq ||
			isl_int_is_zero(bset1->eq[row][col]);
		int is_zero2 = row >= bset2->n_eq ||
			isl_int_is_zero(bset2->eq[row][col]);
		if (!is_zero1 && !is_zero2) {
			set_common_multiple(bset1, bset2, row, col);
			++row;
		} else if (!is_zero1 && is_zero2) {
			construct_column(bset1, bset2, row, col);
		} else if (is_zero1 && !is_zero2) {
			construct_column(bset2, bset1, row, col);
		} else {
			if (transform_column(bset1, bset2, row, col))
				--row;
		}
	}
	isl_basic_set_free(bset2);
	isl_assert(bset1->ctx, row == bset1->n_eq, goto error);
	bset1 = isl_basic_set_normalize_constraints(bset1);
	return bset1;
error:
	isl_basic_set_free(bset1);
	return NULL;
}

/* Find an integer point in the set represented by "tab"
 * that lies outside of the equality "eq" e(x) = 0.
 * If "up" is true, look for a point satisfying e(x) - 1 >= 0.
 * Otherwise, look for a point satisfying -e(x) - 1 >= 0 (i.e., e(x) <= -1).
 * The point, if found, is returned.
 * If no point can be found, a zero-length vector is returned.
 *
 * Before solving an ILP problem, we first check if simply
 * adding the normal of the constraint to one of the known
 * integer points in the basic set represented by "tab"
 * yields another point inside the basic set.
 *
 * The caller of this function ensures that the tableau is bounded or
 * that tab->basis and tab->n_unbounded have been set appropriately.
 */
static struct isl_vec *outside_point(struct isl_tab *tab, isl_int *eq, int up)
{
	struct isl_ctx *ctx;
	struct isl_vec *sample = NULL;
	struct isl_tab_undo *snap;
	unsigned dim;
	int k;

	if (!tab)
		return NULL;
	ctx = tab->mat->ctx;

	dim = tab->n_var;
	sample = isl_vec_alloc(ctx, 1 + dim);
	if (!sample)
		return NULL;
	isl_int_set_si(sample->el[0], 1);
	isl_seq_combine(sample->el + 1,
		ctx->one, tab->bmap->sample->el + 1,
		up ? ctx->one : ctx->negone, eq + 1, dim);
	if (isl_basic_map_contains(tab->bmap, sample))
		return sample;
	isl_vec_free(sample);
	sample = NULL;

	snap = isl_tab_snap(tab);

	if (!up)
		isl_seq_neg(eq, eq, 1 + dim);
	isl_int_sub_ui(eq[0], eq[0], 1);

	if (isl_tab_extend_cons(tab, 1) < 0)
		goto error;
	if (isl_tab_add_ineq(tab, eq) < 0)
		goto error;

	sample = isl_tab_sample(tab);

	isl_int_add_ui(eq[0], eq[0], 1);
	if (!up)
		isl_seq_neg(eq, eq, 1 + dim);

	if (isl_tab_rollback(tab, snap) < 0)
		goto error;

	return sample;
error:
	isl_vec_free(sample);
	return NULL;
}

struct isl_basic_set *isl_basic_set_recession_cone(struct isl_basic_set *bset)
{
	int i;

	bset = isl_basic_set_cow(bset);
	if (!bset)
		return NULL;
	isl_assert(bset->ctx, bset->n_div == 0, goto error);

	for (i = 0; i < bset->n_eq; ++i)
		isl_int_set_si(bset->eq[i][0], 0);

	for (i = 0; i < bset->n_ineq; ++i)
		isl_int_set_si(bset->ineq[i][0], 0);

	ISL_F_CLR(bset, ISL_BASIC_SET_NO_IMPLICIT);
	return isl_basic_set_implicit_equalities(bset);
error:
	isl_basic_set_free(bset);
	return NULL;
}

__isl_give isl_set *isl_set_recession_cone(__isl_take isl_set *set)
{
	int i;

	if (!set)
		return NULL;
	if (set->n == 0)
		return set;

	set = isl_set_remove_divs(set);
	set = isl_set_cow(set);
	if (!set)
		return NULL;

	for (i = 0; i < set->n; ++i) {
		set->p[i] = isl_basic_set_recession_cone(set->p[i]);
		if (!set->p[i])
			goto error;
	}

	return set;
error:
	isl_set_free(set);
	return NULL;
}

/* Extend an initial (under-)approximation of the affine hull of basic
 * set represented by the tableau "tab"
 * by looking for points that do not satisfy one of the equalities
 * in the current approximation and adding them to that approximation
 * until no such points can be found any more.
 *
 * The caller of this function ensures that "tab" is bounded or
 * that tab->basis and tab->n_unbounded have been set appropriately.
 */
static struct isl_basic_set *extend_affine_hull(struct isl_tab *tab,
	struct isl_basic_set *hull)
{
	int i, j, k;
	unsigned dim;

	if (!tab || !hull)
		goto error;

	dim = tab->n_var;

	if (isl_tab_extend_cons(tab, 2 * dim + 1) < 0)
		goto error;

	for (i = 0; i < dim; ++i) {
		struct isl_vec *sample;
		struct isl_basic_set *point;
		for (j = 0; j < hull->n_eq; ++j) {
			sample = outside_point(tab, hull->eq[j], 1);
			if (!sample)
				goto error;
			if (sample->size > 0)
				break;
			isl_vec_free(sample);
			sample = outside_point(tab, hull->eq[j], 0);
			if (!sample)
				goto error;
			if (sample->size > 0)
				break;
			isl_vec_free(sample);

			tab = isl_tab_add_eq(tab, hull->eq[j]);
			if (!tab)
				goto error;
		}
		if (j == hull->n_eq)
			break;
		if (tab->samples)
			tab = isl_tab_add_sample(tab, isl_vec_copy(sample));
		if (!tab)
			goto error;
		point = isl_basic_set_from_vec(sample);
		hull = affine_hull(hull, point);
	}

	return hull;
error:
	isl_basic_set_free(hull);
	return NULL;
}

/* Drop all constraints in bset that involve any of the dimensions
 * first to first+n-1.
 */
static struct isl_basic_set *drop_constraints_involving
	(struct isl_basic_set *bset, unsigned first, unsigned n)
{
	int i;

	if (!bset)
		return NULL;

	bset = isl_basic_set_cow(bset);

	for (i = bset->n_eq - 1; i >= 0; --i) {
		if (isl_seq_first_non_zero(bset->eq[i] + 1 + first, n) == -1)
			continue;
		isl_basic_set_drop_equality(bset, i);
	}

	for (i = bset->n_ineq - 1; i >= 0; --i) {
		if (isl_seq_first_non_zero(bset->ineq[i] + 1 + first, n) == -1)
			continue;
		isl_basic_set_drop_inequality(bset, i);
	}

	return bset;
}

/* Look for all equalities satisfied by the integer points in bset,
 * which is assumed to be bounded.
 *
 * The equalities are obtained by successively looking for
 * a point that is affinely independent of the points found so far.
 * In particular, for each equality satisfied by the points so far,
 * we check if there is any point on a hyperplane parallel to the
 * corresponding hyperplane shifted by at least one (in either direction).
 */
static struct isl_basic_set *uset_affine_hull_bounded(struct isl_basic_set *bset)
{
	struct isl_vec *sample = NULL;
	struct isl_basic_set *hull;
	struct isl_tab *tab = NULL;
	unsigned dim;

	if (isl_basic_set_fast_is_empty(bset))
		return bset;

	dim = isl_basic_set_n_dim(bset);

	if (bset->sample && bset->sample->size == 1 + dim) {
		int contains = isl_basic_set_contains(bset, bset->sample);
		if (contains < 0)
			goto error;
		if (contains) {
			if (dim == 0)
				return bset;
			sample = isl_vec_copy(bset->sample);
		} else {
			isl_vec_free(bset->sample);
			bset->sample = NULL;
		}
	}

	tab = isl_tab_from_basic_set(bset);
	if (!tab)
		goto error;
	if (tab->empty) {
		isl_tab_free(tab);
		isl_vec_free(sample);
		return isl_basic_set_set_to_empty(bset);
	}
	if (isl_tab_track_bset(tab, isl_basic_set_copy(bset)) < 0)
		goto error;

	if (!sample) {
		struct isl_tab_undo *snap;
		snap = isl_tab_snap(tab);
		sample = isl_tab_sample(tab);
		if (isl_tab_rollback(tab, snap) < 0)
			goto error;
		isl_vec_free(tab->bmap->sample);
		tab->bmap->sample = isl_vec_copy(sample);
	}

	if (!sample)
		goto error;
	if (sample->size == 0) {
		isl_tab_free(tab);
		isl_vec_free(sample);
		return isl_basic_set_set_to_empty(bset);
	}

	hull = isl_basic_set_from_vec(sample);

	isl_basic_set_free(bset);
	hull = extend_affine_hull(tab, hull);
	isl_tab_free(tab);

	return hull;
error:
	isl_vec_free(sample);
	isl_tab_free(tab);
	isl_basic_set_free(bset);
	return NULL;
}

/* Given an unbounded tableau and an integer point satisfying the tableau,
 * construct an intial affine hull containing the recession cone
 * shifted to the given point.
 *
 * The unbounded directions are taken from the last rows of the basis,
 * which is assumed to have been initialized appropriately.
 */
static __isl_give isl_basic_set *initial_hull(struct isl_tab *tab,
	__isl_take isl_vec *vec)
{
	int i;
	int k;
	struct isl_basic_set *bset = NULL;
	struct isl_ctx *ctx;
	unsigned dim;

	if (!vec || !tab)
		return NULL;
	ctx = vec->ctx;
	isl_assert(ctx, vec->size != 0, goto error);

	bset = isl_basic_set_alloc(ctx, 0, vec->size - 1, 0, vec->size - 1, 0);
	if (!bset)
		goto error;
	dim = isl_basic_set_n_dim(bset) - tab->n_unbounded;
	for (i = 0; i < dim; ++i) {
		k = isl_basic_set_alloc_equality(bset);
		if (k < 0)
			goto error;
		isl_seq_cpy(bset->eq[k] + 1, tab->basis->row[1 + i] + 1,
			    vec->size - 1);
		isl_seq_inner_product(bset->eq[k] + 1, vec->el +1,
				      vec->size - 1, &bset->eq[k][0]);
		isl_int_neg(bset->eq[k][0], bset->eq[k][0]);
	}
	bset->sample = vec;
	bset = isl_basic_set_gauss(bset, NULL);

	return bset;
error:
	isl_basic_set_free(bset);
	isl_vec_free(vec);
	return NULL;
}

/* Given a tableau of a set and a tableau of the corresponding
 * recession cone, detect and add all equalities to the tableau.
 * If the tableau is bounded, then we can simply keep the
 * tableau in its state after the return from extend_affine_hull.
 * However, if the tableau is unbounded, then
 * isl_tab_set_initial_basis_with_cone will add some additional
 * constraints to the tableau that have to be removed again.
 * In this case, we therefore rollback to the state before
 * any constraints were added and then add the eqaulities back in.
 */
struct isl_tab *isl_tab_detect_equalities(struct isl_tab *tab,
	struct isl_tab *tab_cone)
{
	int j;
	struct isl_vec *sample;
	struct isl_basic_set *hull;
	struct isl_tab_undo *snap;

	if (!tab || !tab_cone)
		goto error;

	snap = isl_tab_snap(tab);

	isl_mat_free(tab->basis);
	tab->basis = NULL;

	isl_assert(tab->mat->ctx, tab->bmap, goto error);
	isl_assert(tab->mat->ctx, tab->samples, goto error);
	isl_assert(tab->mat->ctx, tab->samples->n_col == 1 + tab->n_var, goto error);
	isl_assert(tab->mat->ctx, tab->n_sample > tab->n_outside, goto error);

	if (isl_tab_set_initial_basis_with_cone(tab, tab_cone) < 0)
		goto error;

	sample = isl_vec_alloc(tab->mat->ctx, 1 + tab->n_var);
	if (!sample)
		goto error;

	isl_seq_cpy(sample->el, tab->samples->row[tab->n_outside], sample->size);

	isl_vec_free(tab->bmap->sample);
	tab->bmap->sample = isl_vec_copy(sample);

	if (tab->n_unbounded == 0)
		hull = isl_basic_set_from_vec(isl_vec_copy(sample));
	else
		hull = initial_hull(tab, isl_vec_copy(sample));

	for (j = tab->n_outside + 1; j < tab->n_sample; ++j) {
		isl_seq_cpy(sample->el, tab->samples->row[j], sample->size);
		hull = affine_hull(hull,
				isl_basic_set_from_vec(isl_vec_copy(sample)));
	}

	isl_vec_free(sample);

	hull = extend_affine_hull(tab, hull);
	if (!hull)
		goto error;

	if (tab->n_unbounded == 0) {
		isl_basic_set_free(hull);
		return tab;
	}

	if (isl_tab_rollback(tab, snap) < 0)
		goto error;

	if (hull->n_eq > tab->n_zero) {
		for (j = 0; j < hull->n_eq; ++j) {
			isl_seq_normalize(tab->mat->ctx, hull->eq[j], 1 + tab->n_var);
			tab = isl_tab_add_eq(tab, hull->eq[j]);
		}
	}

	isl_basic_set_free(hull);

	return tab;
error:
	isl_tab_free(tab);
	return NULL;
}

/* Compute the affine hull of "bset", where "cone" is the recession cone
 * of "bset".
 *
 * We first compute a unimodular transformation that puts the unbounded
 * directions in the last dimensions.  In particular, we take a transformation
 * that maps all equalities to equalities (in HNF) on the first dimensions.
 * Let x be the original dimensions and y the transformed, with y_1 bounded
 * and y_2 unbounded.
 *
 *	       [ y_1 ]			[ y_1 ]   [ Q_1 ]
 *	x = U  [ y_2 ]			[ y_2 ] = [ Q_2 ] x
 *
 * Let's call the input basic set S.  We compute S' = preimage(S, U)
 * and drop the final dimensions including any constraints involving them.
 * This results in set S''.
 * Then we compute the affine hull A'' of S''.
 * Let F y_1 >= g be the constraint system of A''.  In the transformed
 * space the y_2 are unbounded, so we can add them back without any constraints,
 * resulting in
 *
 *		        [ y_1 ]
 *		[ F 0 ] [ y_2 ] >= g
 * or
 *		        [ Q_1 ]
 *		[ F 0 ] [ Q_2 ] x >= g
 * or
 *		F Q_1 x >= g
 *
 * The affine hull in the original space is then obtained as
 * A = preimage(A'', Q_1).
 */
static struct isl_basic_set *affine_hull_with_cone(struct isl_basic_set *bset,
	struct isl_basic_set *cone)
{
	unsigned total;
	unsigned cone_dim;
	struct isl_basic_set *hull;
	struct isl_mat *M, *U, *Q;

	if (!bset || !cone)
		goto error;

	total = isl_basic_set_total_dim(cone);
	cone_dim = total - cone->n_eq;

	M = isl_mat_sub_alloc(bset->ctx, cone->eq, 0, cone->n_eq, 1, total);
	M = isl_mat_left_hermite(M, 0, &U, &Q);
	if (!M)
		goto error;
	isl_mat_free(M);

	U = isl_mat_lin_to_aff(U);
	bset = isl_basic_set_preimage(bset, isl_mat_copy(U));

	bset = drop_constraints_involving(bset, total - cone_dim, cone_dim);
	bset = isl_basic_set_drop_dims(bset, total - cone_dim, cone_dim);

	Q = isl_mat_lin_to_aff(Q);
	Q = isl_mat_drop_rows(Q, 1 + total - cone_dim, cone_dim);

	if (bset && bset->sample && bset->sample->size == 1 + total)
		bset->sample = isl_mat_vec_product(isl_mat_copy(Q), bset->sample);

	hull = uset_affine_hull_bounded(bset);

	if (!hull)
		isl_mat_free(U);
	else {
		struct isl_vec *sample = isl_vec_copy(hull->sample);
		U = isl_mat_drop_cols(U, 1 + total - cone_dim, cone_dim);
		if (sample && sample->size > 0)
			sample = isl_mat_vec_product(U, sample);
		else
			isl_mat_free(U);
		hull = isl_basic_set_preimage(hull, Q);
		isl_vec_free(hull->sample);
		hull->sample = sample;
	}

	isl_basic_set_free(cone);

	return hull;
error:
	isl_basic_set_free(bset);
	isl_basic_set_free(cone);
	return NULL;
}

/* Look for all equalities satisfied by the integer points in bset,
 * which is assumed not to have any explicit equalities.
 *
 * The equalities are obtained by successively looking for
 * a point that is affinely independent of the points found so far.
 * In particular, for each equality satisfied by the points so far,
 * we check if there is any point on a hyperplane parallel to the
 * corresponding hyperplane shifted by at least one (in either direction).
 *
 * Before looking for any outside points, we first compute the recession
 * cone.  The directions of this recession cone will always be part
 * of the affine hull, so there is no need for looking for any points
 * in these directions.
 * In particular, if the recession cone is full-dimensional, then
 * the affine hull is simply the whole universe.
 */
static struct isl_basic_set *uset_affine_hull(struct isl_basic_set *bset)
{
	struct isl_basic_set *cone;

	if (isl_basic_set_fast_is_empty(bset))
		return bset;

	cone = isl_basic_set_recession_cone(isl_basic_set_copy(bset));
	if (!cone)
		goto error;
	if (cone->n_eq == 0) {
		struct isl_basic_set *hull;
		isl_basic_set_free(cone);
		hull = isl_basic_set_universe_like(bset);
		isl_basic_set_free(bset);
		return hull;
	}

	if (cone->n_eq < isl_basic_set_total_dim(cone))
		return affine_hull_with_cone(bset, cone);

	isl_basic_set_free(cone);
	return uset_affine_hull_bounded(bset);
error:
	isl_basic_set_free(bset);
	return NULL;
}

/* Look for all equalities satisfied by the integer points in bmap
 * that are independent of the equalities already explicitly available
 * in bmap.
 *
 * We first remove all equalities already explicitly available,
 * then look for additional equalities in the reduced space
 * and then transform the result to the original space.
 * The original equalities are _not_ added to this set.  This is
 * the responsibility of the calling function.
 * The resulting basic set has all meaning about the dimensions removed.
 * In particular, dimensions that correspond to existential variables
 * in bmap and that are found to be fixed are not removed.
 */
static struct isl_basic_set *equalities_in_underlying_set(
						struct isl_basic_map *bmap)
{
	struct isl_mat *T1 = NULL;
	struct isl_mat *T2 = NULL;
	struct isl_basic_set *bset = NULL;
	struct isl_basic_set *hull = NULL;

	bset = isl_basic_map_underlying_set(bmap);
	if (!bset)
		return NULL;
	if (bset->n_eq)
		bset = isl_basic_set_remove_equalities(bset, &T1, &T2);
	if (!bset)
		goto error;

	hull = uset_affine_hull(bset);
	if (!T2)
		return hull;

	if (!hull)
		isl_mat_free(T1);
	else {
		struct isl_vec *sample = isl_vec_copy(hull->sample);
		if (sample && sample->size > 0)
			sample = isl_mat_vec_product(T1, sample);
		else
			isl_mat_free(T1);
		hull = isl_basic_set_preimage(hull, T2);
		isl_vec_free(hull->sample);
		hull->sample = sample;
	}

	return hull;
error:
	isl_mat_free(T2);
	isl_basic_set_free(bset);
	isl_basic_set_free(hull);
	return NULL;
}

/* Detect and make explicit all equalities satisfied by the (integer)
 * points in bmap.
 */
struct isl_basic_map *isl_basic_map_detect_equalities(
						struct isl_basic_map *bmap)
{
	int i, j;
	struct isl_basic_set *hull = NULL;

	if (!bmap)
		return NULL;
	if (bmap->n_ineq == 0)
		return bmap;
	if (ISL_F_ISSET(bmap, ISL_BASIC_MAP_EMPTY))
		return bmap;
	if (ISL_F_ISSET(bmap, ISL_BASIC_MAP_ALL_EQUALITIES))
		return bmap;
	if (ISL_F_ISSET(bmap, ISL_BASIC_MAP_RATIONAL))
		return isl_basic_map_implicit_equalities(bmap);

	hull = equalities_in_underlying_set(isl_basic_map_copy(bmap));
	if (!hull)
		goto error;
	if (ISL_F_ISSET(hull, ISL_BASIC_SET_EMPTY)) {
		isl_basic_set_free(hull);
		return isl_basic_map_set_to_empty(bmap);
	}
	bmap = isl_basic_map_extend_dim(bmap, isl_dim_copy(bmap->dim), 0,
					hull->n_eq, 0);
	for (i = 0; i < hull->n_eq; ++i) {
		j = isl_basic_map_alloc_equality(bmap);
		if (j < 0)
			goto error;
		isl_seq_cpy(bmap->eq[j], hull->eq[i],
				1 + isl_basic_set_total_dim(hull));
	}
	isl_vec_free(bmap->sample);
	bmap->sample = isl_vec_copy(hull->sample);
	isl_basic_set_free(hull);
	ISL_F_SET(bmap, ISL_BASIC_MAP_NO_IMPLICIT | ISL_BASIC_MAP_ALL_EQUALITIES);
	bmap = isl_basic_map_simplify(bmap);
	return isl_basic_map_finalize(bmap);
error:
	isl_basic_set_free(hull);
	isl_basic_map_free(bmap);
	return NULL;
}

__isl_give isl_basic_set *isl_basic_set_detect_equalities(
						__isl_take isl_basic_set *bset)
{
	return (isl_basic_set *)
		isl_basic_map_detect_equalities((isl_basic_map *)bset);
}

struct isl_map *isl_map_detect_equalities(struct isl_map *map)
{
	struct isl_basic_map *bmap;
	int i;

	if (!map)
		return NULL;

	for (i = 0; i < map->n; ++i) {
		bmap = isl_basic_map_copy(map->p[i]);
		bmap = isl_basic_map_detect_equalities(bmap);
		if (!bmap)
			goto error;
		isl_basic_map_free(map->p[i]);
		map->p[i] = bmap;
	}

	return map;
error:
	isl_map_free(map);
	return NULL;
}

__isl_give isl_set *isl_set_detect_equalities(__isl_take isl_set *set)
{
	return (isl_set *)isl_map_detect_equalities((isl_map *)set);
}

/* After computing the rational affine hull (by detecting the implicit
 * equalities), we compute the additional equalities satisfied by
 * the integer points (if any) and add the original equalities back in.
 */
struct isl_basic_map *isl_basic_map_affine_hull(struct isl_basic_map *bmap)
{
	bmap = isl_basic_map_detect_equalities(bmap);
	bmap = isl_basic_map_cow(bmap);
	isl_basic_map_free_inequality(bmap, bmap->n_ineq);
	return bmap;
}

struct isl_basic_set *isl_basic_set_affine_hull(struct isl_basic_set *bset)
{
	return (struct isl_basic_set *)
		isl_basic_map_affine_hull((struct isl_basic_map *)bset);
}

struct isl_basic_map *isl_map_affine_hull(struct isl_map *map)
{
	int i;
	struct isl_basic_map *model = NULL;
	struct isl_basic_map *hull = NULL;
	struct isl_set *set;

	if (!map)
		return NULL;

	if (map->n == 0) {
		hull = isl_basic_map_empty_like_map(map);
		isl_map_free(map);
		return hull;
	}

	map = isl_map_detect_equalities(map);
	map = isl_map_align_divs(map);
	if (!map)
		return NULL;
	model = isl_basic_map_copy(map->p[0]);
	set = isl_map_underlying_set(map);
	set = isl_set_cow(set);
	if (!set)
		goto error;

	for (i = 0; i < set->n; ++i) {
		set->p[i] = isl_basic_set_cow(set->p[i]);
		set->p[i] = isl_basic_set_affine_hull(set->p[i]);
		set->p[i] = isl_basic_set_gauss(set->p[i], NULL);
		if (!set->p[i])
			goto error;
	}
	set = isl_set_remove_empty_parts(set);
	if (set->n == 0) {
		hull = isl_basic_map_empty_like(model);
		isl_basic_map_free(model);
	} else {
		struct isl_basic_set *bset;
		while (set->n > 1) {
			set->p[0] = affine_hull(set->p[0], set->p[--set->n]);
			if (!set->p[0])
				goto error;
		}
		bset = isl_basic_set_copy(set->p[0]);
		hull = isl_basic_map_overlying_set(bset, model);
	}
	isl_set_free(set);
	hull = isl_basic_map_simplify(hull);
	return isl_basic_map_finalize(hull);
error:
	isl_basic_map_free(model);
	isl_set_free(set);
	return NULL;
}

struct isl_basic_set *isl_set_affine_hull(struct isl_set *set)
{
	return (struct isl_basic_set *)
		isl_map_affine_hull((struct isl_map *)set);
}
