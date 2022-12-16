/*++
Copyright (c) 2022 Microsoft Corporation

Module Name:

    polysat univariate solver

Abstract:

    Solve univariate constraints for polysat using bitblasting

Author:

    Nikolaj Bjorner (nbjorner) 2022-03-10
    Jakob Rath 2022-03-10

--*/

#include "math/polysat/univariate/univariate_solver.h"
#include "solver/solver.h"
#include "util/util.h"
#include "ast/ast.h"
#include "ast/reg_decl_plugins.h"
#include "ast/ast_smt2_pp.h"


namespace polysat {

    univariate_solver::dep_vector univariate_solver::unsat_core() {
        dep_vector deps;
        unsat_core(deps);
        return deps;
    }

    class univariate_bitblast_solver : public univariate_solver {
        // TODO: does it make sense to share m and bv between different solver instances?
        // TODO: consider pooling solvers to save setup overhead, see if solver/solver_pool.h can be used
        ast_manager m;
        scoped_ptr<bv_util> bv;
        scoped_ptr<solver> s;
        unsigned bit_width;
        unsigned m_scope_level = 0;
        func_decl_ref x_decl;
        expr_ref x;
        vector<rational> model_cache;

    public:
        univariate_bitblast_solver(solver_factory& mk_solver, unsigned bit_width) :
            bit_width(bit_width),
            x_decl(m),
            x(m) {
            reg_decl_plugins(m);
            bv = alloc(bv_util, m);
            params_ref p;
            p.set_bool("bv.polysat", false);
            s = mk_solver(m, p, false, true, true, symbol::null);
            x_decl = m.mk_const_decl("x", bv->mk_sort(bit_width));
            x = m.mk_const(x_decl);
            model_cache.push_back(rational(-1));
        }

        ~univariate_bitblast_solver() override = default;

        void reset_cache() {
            model_cache.back() = -1;
        }

        void push_cache() {
            model_cache.push_back(model_cache.back());
        }

        void pop_cache() {
            model_cache.pop_back();
        }

        void push() override {
            m_scope_level++;
            push_cache();
            s->push();
        }

        void pop(unsigned n) override {
            SASSERT(scope_level() >= n);
            m_scope_level -= n;
            pop_cache();
            s->pop(n);
        }

        unsigned scope_level() override {
            return m_scope_level;
        }

        expr* mk_numeral(rational const& r) const {
            return bv->mk_numeral(r, bit_width);
        }

#if 0
        // [d,c,b,a]  -->  ((a*x + b)*x + c)*x + d
        expr* mk_poly(univariate const& p) const {
            if (p.empty()) {
                return mk_numeral(rational::zero());
            }
            else {
                expr* e = mk_numeral(p.back());
                for (unsigned i = p.size() - 1; i-- > 0; ) {
                    e = bv->mk_bv_mul(e, x);
                    if (!p[i].is_zero())
                        e = bv->mk_bv_add(e, mk_numeral(p[i]));
                }
                return e;
            }
        }
#else
        // TODO: shouldn't the simplification step of the underlying solver already support this transformation? how to enable?
        // 2^k*x  -->  x << k
        // n*x    -->  n * x
        expr* mk_poly_term(rational const& coeff, expr* xpow) const {
            unsigned pow;
            if (coeff.is_power_of_two(pow))
                return bv->mk_bv_shl(xpow, mk_numeral(rational(pow)));
            else
                return bv->mk_bv_mul(mk_numeral(coeff), xpow);
        }

        // [d,c,b,a]  -->  d + c*x + b*(x*x) + a*(x*x*x)
        expr* mk_poly(univariate const& p) const {
            if (p.empty()) {
                return mk_numeral(rational::zero());
            }
            else {
                expr* e = mk_numeral(p[0]);
                expr* xpow = x;
                for (unsigned i = 1; i < p.size(); ++i) {
                    if (!p[i].is_zero()) {
                        expr* t = mk_poly_term(p[i], xpow);
                        e = bv->mk_bv_add(e, t);
                    }
                    if (i + 1 < p.size())
                        xpow = bv->mk_bv_mul(xpow, x);
                }
                return e;
            }
        }
#endif

        void add(expr* e, bool sign, dep_t dep) {
            reset_cache();
            if (sign)
                e = m.mk_not(e);
            expr* a = m.mk_const(m.mk_const_decl(symbol(dep), m.mk_bool_sort()));
            s->assert_expr(e, a);
            IF_VERBOSE(10, verbose_stream() << "(assert (! " << expr_ref(e, m) << "      :named " << expr_ref(a, m) << "))\n");
        }

        void add_ule(univariate const& lhs, univariate const& rhs, bool sign, dep_t dep) override {
            add(bv->mk_ule(mk_poly(lhs), mk_poly(rhs)), sign, dep);
        }

        void add_umul_ovfl(univariate const& lhs, univariate const& rhs, bool sign, dep_t dep) override {
            add(bv->mk_bvumul_no_ovfl(mk_poly(lhs), mk_poly(rhs)), !sign, dep);
        }

        void add_smul_ovfl(univariate const& lhs, univariate const& rhs, bool sign, dep_t dep) override {
            add(bv->mk_bvsmul_no_ovfl(mk_poly(lhs), mk_poly(rhs)), !sign, dep);
        }

        void add_smul_udfl(univariate const& lhs, univariate const& rhs, bool sign, dep_t dep) override {
            add(bv->mk_bvsmul_no_udfl(mk_poly(lhs), mk_poly(rhs)), !sign, dep);
        }

        void add_lshr(univariate const& in1, univariate const& in2, univariate const& out, bool sign, dep_t dep) override {
            add(m.mk_eq(bv->mk_bv_lshr(mk_poly(in1), mk_poly(in2)), mk_poly(out)), sign, dep);
        }

        void add_ashr(univariate const& in1, univariate const& in2, univariate const& out, bool sign, dep_t dep) override {
            add(m.mk_eq(bv->mk_bv_ashr(mk_poly(in1), mk_poly(in2)), mk_poly(out)), sign, dep);
        }

        void add_shl(univariate const& in1, univariate const& in2, univariate const& out, bool sign, dep_t dep) override {
            add(m.mk_eq(bv->mk_bv_shl(mk_poly(in1), mk_poly(in2)), mk_poly(out)), sign, dep);
        }

        void add_and(univariate const& in1, univariate const& in2, univariate const& out, bool sign, dep_t dep) override {
            add(m.mk_eq(bv->mk_bv_and(mk_poly(in1), mk_poly(in2)), mk_poly(out)), sign, dep);
        }

        void add_or(univariate const& in1, univariate const& in2, univariate const& out, bool sign, dep_t dep) override {
            add(m.mk_eq(bv->mk_bv_or(mk_poly(in1), mk_poly(in2)), mk_poly(out)), sign, dep);
        }

        void add_xor(univariate const& in1, univariate const& in2, univariate const& out, bool sign, dep_t dep) override {
            add(m.mk_eq(bv->mk_bv_xor(mk_poly(in1), mk_poly(in2)), mk_poly(out)), sign, dep);
        }

        void add_not(univariate const& in, univariate const& out, bool sign, dep_t dep) override {
            add(m.mk_eq(bv->mk_bv_not(mk_poly(in)), mk_poly(out)), sign, dep);
        }

        void add_ule_const(rational const& val, bool sign, dep_t dep) override {
            add(bv->mk_ule(x, mk_numeral(val)), sign, dep);
        }

        void add_uge_const(rational const& val, bool sign, dep_t dep) override {
            add(bv->mk_ule(mk_numeral(val), x), sign, dep);
        }

        void add_bit(unsigned idx, bool sign, dep_t dep) override {
            add(bv->mk_bit2bool(x, idx), sign, dep);
        }

        lbool check() override {
            return s->check_sat();
        }

        void unsat_core(dep_vector& deps) override {
            deps.reset();
            expr_ref_vector core(m);
            s->get_unsat_core(core);
            for (expr* a : core) {
                unsigned dep = to_app(a)->get_decl()->get_name().get_num();
                deps.push_back(dep);
            }
            SASSERT(deps.size() > 0);
        }

        rational model() override {
            rational& cached_model = model_cache.back();
            if (cached_model.is_neg()) {
                model_ref model;
                s->get_model(model);
                SASSERT(model);
                app* val = to_app(model->get_const_interp(x_decl));
                SASSERT(val->get_decl_kind() == OP_BV_NUM);
                SASSERT(val->get_num_parameters() == 2);
                auto const& p = val->get_parameter(0);
                SASSERT(p.is_rational());
                cached_model = p.get_rational();
            }
            return cached_model;
        }

        bool find_min(rational& val) override {
            val = model();
            push();
            // try reducing val by setting bits to 0, starting at the msb.
            for (unsigned k = bit_width; k-- > 0; ) {
                if (!val.get_bit(k)) {
                    add_bit0(k, 0);
                    continue;
                }
                // try decreasing k-th bit
                push();
                add_bit0(k, 0);
                lbool result = check();
                if (result == l_true) {
                    SASSERT(model() < val);
                    val = model();
                }
                pop(1);
                if (result == l_true)
                    add_bit0(k, 0);
                else if (result == l_false)
                    add_bit1(k, 0);
                else
                    return false;
            }
            pop(1);
            return true;
        }

        bool find_max(rational& val) override {
            val = model();
            push();
            // try increasing val by setting bits to 1, starting at the msb.
            for (unsigned k = bit_width; k-- > 0; ) {
                if (val.get_bit(k)) {
                    add_bit1(k, 0);
                    continue;
                }
                // try increasing k-th bit
                push();
                add_bit1(k, 0);
                lbool result = check();
                if (result == l_true) {
                    SASSERT(model() > val);
                    val = model();
                }
                pop(1);
                if (result == l_true)
                    add_bit1(k, 0);
                else if (result == l_false)
                    add_bit0(k, 0);
                else
                    return false;
            }
            pop(1);
            return true;
        }

        std::ostream& display(std::ostream& out) const override {
            return out << *s;
        }
    };

    class univariate_bitblast_factory : public univariate_solver_factory {
        symbol qf_bv;
        scoped_ptr<solver_factory> sf;

    public:
        univariate_bitblast_factory() :
            qf_bv("QF_BV") {
            sf = mk_smt_strategic_solver_factory(qf_bv);
        }

        ~univariate_bitblast_factory() override = default;

        scoped_ptr<univariate_solver> operator()(unsigned bit_width) override {
            return alloc(univariate_bitblast_solver, *sf, bit_width);
        }
    };

    scoped_ptr<univariate_solver_factory> mk_univariate_bitblast_factory() {
        return alloc(univariate_bitblast_factory);
    }
}
