/*++
Copyright (c) 2014 Microsoft Corporation

Module Name:

    fixplex.h

Abstract:

    Fixed-precision unsigned integer simplex tableau.

Author:

    Nikolaj Bjorner (nbjorner) 2021-03-19
    Jakob Rath 2021-04-6

--*/

#pragma once

#include <limits>
#include "math/simplex/sparse_matrix.h"
#include "util/heap.h"
#include "util/map.h"
#include "util/lbool.h"
#include "util/uint_set.h"

namespace polysat {

    typedef unsigned var_t;

    template<typename Numeral>
    struct pp {
        Numeral n;
        pp(Numeral const& n):n(n) {}
    };

    template<typename Numeral>
    inline std::ostream& operator<<(std::ostream& out, pp<Numeral> const& p) {
        if ((0 - p.n) < p.n)
            return out << "-" << (0 - p.n);
        return out << p.n;
    }

    /**
     * Modular interval arithmetic
     */
    template<typename Numeral>
    struct interval {
        bool emp { false };
        Numeral lo { 0 };
        Numeral hi { 0 };
        interval() {}
        interval(Numeral const& l, Numeral const& h): lo(l), hi(h) {}
        static interval free() { return interval(0, 0); }
        static interval empty() { interval i(0, 0); i.emp = true; return i; }
        bool is_free() const { return !emp && lo == hi; }
        bool is_empty() const { return emp; }
        bool contains(Numeral const& n) const;
        interval operator&(interval const& other) const;
        interval operator+(interval const& other) const;
        interval operator-(interval const& other) const;
        interval operator*(interval const& other) const;
        interval operator-() const;
        interval operator*(Numeral const& n) const;
        interval operator+(Numeral const& n) const { return interval(lo + n, hi + n); }
        interval operator-(Numeral const& n) const { return interval(lo - n, hi - n); }
        interval& operator+=(interval const& other) { *this = *this + other; return *this; }
        std::ostream& display(std::ostream& out) const { 
            if (is_empty()) return out << "empty"; 
            if (is_free()) return out << "free";
            return out << "[" << pp(lo) << ", " << pp(hi) << "["; 
        }
    };

    template<typename Numeral>
    inline std::ostream& operator<<(std::ostream& out, interval<Numeral> const& i) {
        return i.display(out);
    }

    template<typename Ext>
    class fixplex {
    public:
        typedef typename Ext::numeral numeral;
        typedef typename Ext::scoped_numeral scoped_numeral;
        typedef typename Ext::manager manager;
        typedef simplex::sparse_matrix<Ext> matrix;
        typedef typename matrix::row row;
        typedef typename matrix::row_iterator row_iterator;
        typedef typename matrix::col_iterator col_iterator;
    protected:
        struct var_lt {
            bool operator()(var_t v1, var_t v2) const { return v1 < v2; }
        };
        typedef heap<var_lt> var_heap;

        struct stats {
            unsigned m_num_pivots;
            unsigned m_num_infeasible;
            unsigned m_num_checks;
            stats() { reset(); }
            void reset() {
                memset(this, 0, sizeof(*this));
            }
        };

        enum pivot_strategy_t {
            S_BLAND,
            S_GREATEST_ERROR,
            S_LEAST_ERROR,
            S_DEFAULT
        };

        struct var_info : public interval<numeral> {
            unsigned    m_base2row:29;
            unsigned    m_is_base:1;
            numeral     m_value { 0 };
            var_info():
                m_base2row(0),
                m_is_base(false)
            {}
            var_info& operator&=(interval<numeral> const& range) {
                interval<numeral>::operator=(range);
                return *this;
            }
        };

        struct row_info {
            bool    m_integral { true };
            var_t   m_base;
            numeral m_value;
            numeral m_base_coeff;            
        };

        struct var_eq {
            var_t x, y;
            row r1, r2;
            var_eq(var_t x, var_t y, row const& r1, row const& r2):
                x(x), y(y), r1(r1), r2(r2) {}
        };

        struct fix_entry {
            var_t x;
            row r;
            fix_entry(var_t x, row const& r): x(x), r(r) {}
            fix_entry():x(null_var), r(0) {}
        };

        static const var_t null_var = UINT_MAX;
        reslimit&                   m_limit;
        mutable manager             m;
        mutable matrix              M;
        unsigned                    m_max_iterations { UINT_MAX };
        unsigned                    m_num_non_integral { 0 };
        var_heap                    m_to_patch;
        vector<var_info>            m_vars;
        vector<row_info>            m_rows;
        vector<var_eq>              m_var_eqs;
        bool                        m_bland { false };
        unsigned                    m_blands_rule_threshold { 1000 };
        random_gen                  m_random;
        uint_set                    m_left_basis;
        unsigned                    m_infeasible_var { null_var };
        unsigned_vector             m_base_vars;
        stats                       m_stats;
        map<numeral, fix_entry, typename manager::hash, typename manager::eq> m_value2fixed_var;

    public:
        fixplex(reslimit& lim):
            m_limit(lim),
            M(m),
            m_to_patch(1024) {}

        ~fixplex();


        void set_bounds(var_t v, numeral const& lo, numeral const& hi);
        void unset_bounds(var_t v) { m_vars[v].lo = m_vars[v].hi; }

        var_t get_base_var(row const& r) const { return m_rows[r.id()].m_base; }
        numeral const& lo(var_t var) const { return m_vars[var].lo; }
        numeral const& hi(var_t var) const { return m_vars[var].hi; }
        numeral const& value(var_t var) const { return m_vars[var].m_value; }
        void set_max_iterations(unsigned n) { m_max_iterations = n; }
        unsigned get_num_vars() const { return m_vars.size(); }
        void  reset();
        void  propagate_bounds();
        lbool make_feasible();
        row add_row(var_t base, unsigned num_vars, var_t const* vars, numeral const* coeffs);
        std::ostream& display(std::ostream& out) const;
        std::ostream& display_row(std::ostream& out, row const& r, bool values = true);
        void collect_statistics(::statistics & st) const;

        row get_infeasible_row();

        void del_row(var_t base_var);


    private:

        void gauss_jordan();
        void make_basic(var_t v, row const& r);

        void update_value_core(var_t v, numeral const& delta);
        void  ensure_var(var_t v);

        var_t select_smallest_var() { return m_to_patch.empty()?null_var:m_to_patch.erase_min(); }
        lbool make_var_feasible(var_t x_i);
        bool is_infeasible_row(var_t x);
        bool is_parity_infeasible_row(var_t x);
        bool is_offset_row(row const& r, numeral& cx, var_t& x, numeral& cy, var_t & y) const;
        void lookahead_eq(row const& r1, numeral const& cx, var_t x, numeral const& cy, var_t y);
        void get_offset_eqs(row const& r);
        void fixed_var_eh(row const& r, var_t x);
        void eq_eh(var_t x, var_t y, row const& r1, row const& r2);
        void propagate_bounds(row const& r);
        void new_bound(row const& r, var_t x, interval<numeral> const& range);
        void pivot(var_t x_i, var_t x_j, numeral const& b, numeral const& value);
        numeral value2delta(var_t v, numeral const& new_value) const;
        void update_value(var_t v, numeral const& delta);
        bool can_pivot(var_t x_i, numeral const& new_value, numeral const& a_ij, var_t x_j);
        bool has_minimal_trailing_zeros(var_t y, numeral const& b);
        var_t select_pivot_core(var_t x, numeral const& new_value, numeral& out_b);
        bool in_bounds(var_t v) const { return in_bounds(v, value(v)); }
        bool in_bounds(var_t v, numeral const& b) const { return in_bounds(b, m_vars[v]); }
        bool in_bounds(numeral const& val, interval<numeral> const& range) const { return range.contains(val); }
        bool is_free(var_t v) const { return lo(v) == hi(v); }
        bool is_non_free(var_t v) const { return !is_free(v); }
        bool is_fixed(var_t v) const { return lo(v) + 1 == hi(v); }
        bool is_valid_variable(var_t v) const { return v < m_vars.size(); }
        bool is_base(var_t x) const { return m_vars[x].m_is_base; }
        unsigned base2row(var_t x) const { return m_vars[x].m_base2row; }
        numeral const& row2value(row const& r) const { return m_rows[r.id()].m_value; }
        numeral const& row2base_coeff(row const& r) const { return m_rows[r.id()].m_base_coeff; }
        var_t row2base(row const& r) const { return m_rows[r.id()].m_base; }
        bool row2integral(row const& r) const { return m_rows[r.id()].m_integral; }
        void set_base_value(var_t x); 
        numeral solve_for(numeral const& row_value, numeral const& coeff);
        bool is_feasible() const;
        int get_num_non_free_dep_vars(var_t x_j, int best_so_far);
        void add_patch(var_t v);
        var_t select_var_to_fix();
        void check_blands_rule(var_t v, unsigned& num_repeated);
        pivot_strategy_t pivot_strategy() { return m_bland ? S_BLAND : S_DEFAULT; }
        var_t select_error_var(bool least);

        bool is_solved(row const& r) const;
        bool is_solved(var_t v) const { SASSERT(is_base(v)); return is_solved(base2row(v)); }

        bool well_formed() const;                 
        bool well_formed_row(row const& r) const;

        void  del_row(row const& r);


#if 0
        // TBD: 
        void move_to_bound(var_t x, bool to_lower) {}
        var_t select_pivot(var_t x_i, bool is_below, numeral& out_a_ij) { throw nullptr; }
        var_t select_pivot_blands(var_t x_i, bool is_below, numeral& out_a_ij) { throw nullptr; }
        var_t pick_var_to_leave(var_t x_j, bool is_pos, 
                                numeral& gain, numeral& new_a_ij, bool& inc) { throw nullptr; }

#endif

    };


    struct uint64_ext {
        typedef uint64_t numeral;

        struct manager {
            typedef uint64_t numeral;
            struct hash {
                unsigned operator()(numeral const& n) const { 
                    return static_cast<unsigned>(n); 
                }
            };
            struct eq {
                bool operator()(numeral const& a, numeral const& b) const {
                    return a == b;
                }
            };
            void reset() {}
            void reset(numeral& n) { n = 0; }
            void del(numeral const& n) {}
            bool is_zero(numeral const& n) const { return n == 0; }
            bool is_one(numeral const& n) const { return n == 1; }
            bool is_even(numeral const& n) const { return (n & 1) == 0; }
            bool is_minus_one(numeral const& n) const { return n + 1 == 0; }
            void add(numeral const& a, numeral const& b, numeral& r) { r = a + b; }
            void sub(numeral const& a, numeral const& b, numeral& r) { r = a - b; }
            void mul(numeral const& a, numeral const& b, numeral& r) { r = a * b; }
            void set(numeral& r, numeral const& a) { r = a; }
            void neg(numeral& a) { a = 0 - a; }
            numeral inv(numeral const& a) { return 0 - a; }
            void swap(numeral& a, numeral& b) { std::swap(a, b); }
            unsigned trailing_zeros(numeral const& a) const { return ::trailing_zeros(a); }
            numeral mul_inverse(numeral const& x) {
                if (x == 0)
                    return 0;
                numeral t0 = 1, t1 = 0 - 1;
                numeral r0 = x, r1 = 0 - x;
                while (r1 != 0) {
                    numeral q = r0 / r1;
                    numeral tmp = t1;
                    t1 = t0 - q * t1;
                    t0 = tmp;
                    tmp = r1;
                    r1 = r0 - q * r1;
                    r0 = tmp;
                }
                return t0;
            }
            numeral gcd(numeral x, numeral y) {
                if (x == 0) 
                    return y;
                if (y == 0)
                    return x;
                unsigned tz = trailing_zeros(x);
                numeral shift = std::min(trailing_zeros(y), tz);
                x >>= tz;
                if (x == 1) 
                    return x << shift;
                if (y == 1) 
                    return y << shift;
                if (x == y) 
                    return x << shift;
                do {
                    tz = trailing_zeros(y);
                    y >>= tz;
                    if (x > y) 
                        swap(x, y);
                    y -= x;
                }
                while (y != 0);
                return x << shift;
            }

            std::ostream& display(std::ostream& out, numeral const& x) const { 
                return out << pp(x); 
            }
        };
        typedef _scoped_numeral<manager> scoped_numeral;
    };


    template<typename Ext>
    inline std::ostream& operator<<(std::ostream& out, fixplex<Ext> const& fp) {
        return fp.display(out);
    }


};

