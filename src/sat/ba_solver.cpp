/*++
Copyright (c) 2017 Microsoft Corporation

Module Name:

    ba_solver.cpp

Abstract:

    Extension for cardinality and xor reasoning.

Author:

    Nikolaj Bjorner (nbjorner) 2017-01-30

Revision History:

--*/

#include "sat/ba_solver.h"
#include "sat/sat_types.h"
#include "util/lp/lar_solver.h"


namespace sat {

    static unsigned _bad_id = 11111111; // 2759; //
#define BADLOG(_cmd_) if (p.id() == _bad_id) { _cmd_; }

    ba_solver::card& ba_solver::constraint::to_card() {
        SASSERT(is_card());
        return static_cast<card&>(*this);
    }

    ba_solver::card const& ba_solver::constraint::to_card() const{
        SASSERT(is_card());
        return static_cast<card const&>(*this);
    }

    ba_solver::pb& ba_solver::constraint::to_pb() {
        SASSERT(is_pb());
        return static_cast<pb&>(*this);
    }

    ba_solver::pb const& ba_solver::constraint::to_pb() const{
        SASSERT(is_pb());
        return static_cast<pb const&>(*this);
    }

    ba_solver::xor& ba_solver::constraint::to_xor() {
        SASSERT(is_xor());
        return static_cast<xor&>(*this);
    }

    ba_solver::xor const& ba_solver::constraint::to_xor() const{
        SASSERT(is_xor());
        return static_cast<xor const&>(*this);
    }

    std::ostream& operator<<(std::ostream& out, ba_solver::constraint const& cnstr) {
        if (cnstr.lit() != null_literal) out << cnstr.lit() << " == ";
        switch (cnstr.tag()) {
        case ba_solver::card_t: {
            ba_solver::card const& c = cnstr.to_card();
            for (literal l : c) {
                out << l << " ";
            }
            out << " >= " << c.k();
            break;
        }
        case ba_solver::pb_t: {
            ba_solver::pb const& p = cnstr.to_pb();
            for (ba_solver::wliteral wl : p) {
                if (wl.first != 1) out << wl.first << " * ";
                out << wl.second << " ";
            }
            out << " >= " << p.k();
            break;
        }
        case ba_solver::xor_t: {
            ba_solver::xor const& x = cnstr.to_xor();
            for (unsigned i = 0; i < x.size(); ++i) {
                out << x[i] << " ";
                if (i + 1 < x.size()) out << "x ";
            }            
            break;
        }
        default:
            UNREACHABLE();
        }
        return out;
    }


    // -----------------------
    // pb_base

    bool ba_solver::pb_base::well_formed() const {
        uint_set vars;        
        if (lit() != null_literal) vars.insert(lit().var());
        for (unsigned i = 0; i < size(); ++i) {
            bool_var v = get_lit(i).var();
            if (vars.contains(v)) return false;
            if (get_coeff(i) > k()) return false;
            vars.insert(v);
        }
        return true;
    }

    // ----------------------
    // card

    ba_solver::card::card(unsigned id, literal lit, literal_vector const& lits, unsigned k):
        pb_base(card_t, id, lit, lits.size(), get_obj_size(lits.size()), k) {
        for (unsigned i = 0; i < size(); ++i) {
            m_lits[i] = lits[i];
        }
    }

    void ba_solver::card::negate() {
        m_lit.neg();
        for (unsigned i = 0; i < m_size; ++i) {
            m_lits[i].neg();
        }
        m_k = m_size - m_k + 1;
        SASSERT(m_size >= m_k && m_k > 0);
    }

    bool ba_solver::card::is_watching(literal l) const {
        unsigned sz = std::min(k() + 1, size());
        for (unsigned i = 0; i < sz; ++i) {
            if ((*this)[i] == l) return true;
        }
        return false;
    }

    // -----------------------------------
    // pb

    ba_solver::pb::pb(unsigned id, literal lit, svector<ba_solver::wliteral> const& wlits, unsigned k):
        pb_base(pb_t, id, lit, wlits.size(), get_obj_size(wlits.size()), k),
        m_slack(0),
        m_num_watch(0),
        m_max_sum(0) {
        for (unsigned i = 0; i < size(); ++i) {
            m_wlits[i] = wlits[i];
        }
        update_max_sum();
    }

    void ba_solver::pb::update_max_sum() {
        m_max_sum = 0;
        for (unsigned i = 0; i < size(); ++i) {
            m_wlits[i].first = std::min(k(), m_wlits[i].first);
            if (m_max_sum + m_wlits[i].first < m_max_sum) {
                throw default_exception("addition of pb coefficients overflows");
            }
            m_max_sum += m_wlits[i].first;
        }
    }

    void ba_solver::pb::negate() {
        m_lit.neg();
        unsigned w = 0;
        for (unsigned i = 0; i < m_size; ++i) {
            m_wlits[i].second.neg();
            w += m_wlits[i].first;
        }
        m_k = w - m_k + 1;
        SASSERT(w >= m_k && m_k > 0);
    }

    bool ba_solver::pb::is_watching(literal l) const {
        for (unsigned i = 0; i < m_num_watch; ++i) {
            if ((*this)[i].second == l) return true;
        }
        return false;
    }


    bool ba_solver::pb::is_cardinality() const {
        if (size() == 0) return false;
        unsigned w = (*this)[0].first;
        for (wliteral wl : *this) if (w != wl.first) return false;
        return true;
    }


    // -----------------------------------
    // xor
    
    ba_solver::xor::xor(unsigned id, literal lit, literal_vector const& lits):
    constraint(xor_t, id, lit, lits.size(), get_obj_size(lits.size())) {
        for (unsigned i = 0; i < size(); ++i) {
            m_lits[i] = lits[i];
        }
    }

    bool ba_solver::xor::is_watching(literal l) const {
        return 
            l == (*this)[0] || l == (*this)[1] ||
            ~l == (*this)[0] || ~l == (*this)[1];            
    }

    bool ba_solver::xor::well_formed() const {
        uint_set vars;        
        if (lit() != null_literal) vars.insert(lit().var());
        for (literal l : *this) {
            bool_var v = l.var();
            if (vars.contains(v)) return false;
            vars.insert(v);            
        }
        return true;
    }

    // ----------------------------
    // card

    bool ba_solver::init_watch(card& c, bool is_true) {
        clear_watch(c);
        if (c.lit() != null_literal && c.lit().sign() == is_true) {
            c.negate();
        }
        TRACE("sat", display(tout << "init watch: ", c, true););
        SASSERT(c.lit() == null_literal || value(c.lit()) == l_true);
        unsigned j = 0, sz = c.size(), bound = c.k();
        // put the non-false literals into the head.

        if (bound == sz) {
            for (literal l : c) assign(c, l);
            return false;
        }

        for (unsigned i = 0; i < sz; ++i) {
            if (value(c[i]) != l_false) {
                if (j != i) {
                    c.swap(i, j);
                }
                ++j;
            }
        }
        DEBUG_CODE(
            bool is_false = false;
            for (literal l : c) {
                SASSERT(!is_false || value(l) == l_false);
                is_false = value(l) == l_false;
            });

        // j is the number of non-false, sz - j the number of false.

        if (j < bound) {
            SASSERT(0 < bound && bound < sz);
            literal alit = c[j];
            
            //
            // we need the assignment level of the asserting literal to be maximal.
            // such that conflict resolution can use the asserting literal as a starting
            // point.
            //

            for (unsigned i = bound; i < sz; ++i) {                
                if (lvl(alit) < lvl(c[i])) {
                    c.swap(i, j);
                    alit = c[j];
                }
            }
            set_conflict(c, alit);
            return false;
        }
        else if (j == bound) {
            for (unsigned i = 0; i < bound; ++i) {
                assign(c, c[i]);                
            }
            return false;
        }
        else {
            for (unsigned i = 0; i <= bound; ++i) {
                watch_literal(c[i], c);
            }
            return true;
        }
    }

    void ba_solver::clear_watch(card& c) {
        unsigned sz = std::min(c.k() + 1, c.size());
        for (unsigned i = 0; i < sz; ++i) {
            unwatch_literal(c[i], c);            
        }
    }

    // -----------------------
    // constraint

    void ba_solver::set_conflict(constraint& c, literal lit) {
        m_stats.m_num_conflicts++;
        TRACE("sat", display(tout, c, true); );
        SASSERT(validate_conflict(c));
        if (c.is_xor() && value(lit) == l_true) lit.neg();
        SASSERT(value(lit) == l_false);
        set_conflict(justification::mk_ext_justification(c.index()), ~lit);
        SASSERT(inconsistent());
    }

    void ba_solver::assign(constraint& c, literal lit) {
        if (inconsistent()) return;
        switch (value(lit)) {
        case l_true: 
            break;
        case l_false: 
            set_conflict(c, lit); 
            break;
        default:
            m_stats.m_num_propagations++;
            m_num_propagations_since_pop++;
            //TRACE("sat", tout << "#prop: " << m_stats.m_num_propagations << " - " << c.lit() << " => " << lit << "\n";);
            SASSERT(validate_unit_propagation(c, lit));
            if (get_config().m_drat) {
                svector<drat::premise> ps;
                literal_vector lits;
                get_antecedents(lit, c, lits);
                lits.push_back(lit);
                ps.push_back(drat::premise(drat::s_ext(), c.lit())); // null_literal case.
                drat_add(lits, ps);
            }
            assign(lit, justification::mk_ext_justification(c.index()));
            break;
        }
    }

    // -------------------
    // pb_base

    void ba_solver::simplify(pb_base& p) {
        SASSERT(s().at_base_lvl());
        if (p.lit() != null_literal && value(p.lit()) == l_false) {
            TRACE("sat", tout << "pb: flip sign " << p << "\n";);
            IF_VERBOSE(0, verbose_stream() << "sign is flipped " << p << "\n";);
            return;
            init_watch(p, !p.lit().sign());
        }
        bool nullify = p.lit() != null_literal && value(p.lit()) == l_true;
        if (nullify) {
            SASSERT(lvl(p.lit()) == 0);
            nullify_tracking_literal(p);
        }

        SASSERT(p.lit() == null_literal || value(p.lit()) == l_undef);

        unsigned true_val = 0, slack = 0, num_false = 0;
        for (unsigned i = 0; i < p.size(); ++i) {
            literal l = p.get_lit(i);
            switch (value(l)) {
            case l_true: true_val += p.get_coeff(i); break;
            case l_false: ++num_false; break;
            default: slack += p.get_coeff(i); break;
            }
        }
        if (p.k() == 1 && p.lit() == null_literal) {
            literal_vector lits(p.literals());
            s().mk_clause(lits.size(), lits.c_ptr(), p.learned());
            remove_constraint(p);
        }
        else if (true_val == 0 && num_false == 0) {
            if (nullify) {
                init_watch(p, true);
            }
        }
        else if (true_val >= p.k()) {
            if (p.lit() != null_literal) {
                s().assign(p.lit(), justification());
            }        
            remove_constraint(p);
        }
        else if (slack + true_val < p.k()) {
            if (p.lit() != null_literal) {
                s().assign(~p.lit(), justification());
            }
            else {
                IF_VERBOSE(0, verbose_stream() << "unsat during simplification\n";);
                s().set_conflict(justification());
            }
            remove_constraint(p);
        }
        else if (slack + true_val == p.k()) {
            literal_vector lits(p.literals());
            assert_unconstrained(p.lit(), lits);
            remove_constraint(p);
        }
        else {
            unsigned sz = p.size();
            clear_watch(p);
            for (unsigned i = 0; i < sz; ++i) {
                literal l = p.get_lit(i);
                if (value(l) != l_undef) {
                    --sz;
                    p.swap(i, sz);
                    --i;
                }
            }
            BADLOG(display(verbose_stream() << "simplify ", p, true));
            p.set_size(sz);
            p.set_k(p.k() - true_val);
            BADLOG(display(verbose_stream() << "simplified ", p, true));
            // display(verbose_stream(), c, true);

            if (p.k() == 1 && p.lit() == null_literal) {
                literal_vector lits(p.literals());
                s().mk_clause(lits.size(), lits.c_ptr(), p.learned());
                remove_constraint(p);
                return;
            }
            else if (p.lit() == null_literal) {
                init_watch(p, true);
            }
            else {
                SASSERT(value(p.lit()) == l_undef);
            }
            SASSERT(p.well_formed());
            if (p.is_pb()) simplify2(p.to_pb());
            m_simplify_change = true;
        }
    }

    /*
      \brief slit PB constraint into two because root is reused in arguments.
    
      x <=> a*x + B*y >= k
     
      x => a*x + By >= k
      ~x => a*x + By < k
     
      k*~x + a*x + By >= k
      (B+a-k + 1)*x + a*~x + B*~y >= B + a - k + 1

      (k - a) * ~x + By >= k - a
      k' * x + B'y >= k'
       
    */
    
    void ba_solver::split_root(pb_base& p) {
        SASSERT(p.lit() != null_literal);
        SASSERT(!p.learned());
        m_weights.resize(2*s().num_vars(), 0);
        unsigned k = p.k();
        unsigned w, w1, w2;
        literal root = p.lit();
        m_weights[(~root).index()] = k;
        for (unsigned i = 0; i < p.size(); ++i) {
            m_weights[p.get_lit(i).index()] += p.get_coeff(i);
        }
        literal_vector lits(p.literals());
        lits.push_back(~root);

        for (literal l : lits) {
            w1 = m_weights[l.index()];
            w2 = m_weights[(~l).index()];
            if (w1 >= w2) {
                if (w2 >= k) {
                    // constraint is true
                    return;
                }
                k -= w2;
                m_weights[(~l).index()] = 0;
                m_weights[l.index()] = w1 - w2;
            }
        }
        SASSERT(k > 0);

        // ~root * (k - a) + p >= k - a

        m_wlits.reset();
        for (literal l : lits) {
            w = m_weights[l.index()];
            if (w != 0) {
                m_wlits.push_back(wliteral(w, l));
            } 
            m_weights[l.index()] = 0;
        }
        
        add_pb_ge(null_literal, m_wlits, k, false);        
    }


    // -------------------
    // pb


    // watch a prefix of literals, such that the slack of these is >= k
    bool ba_solver::init_watch(pb& p, bool is_true) {
        clear_watch(p);
        if (p.lit() != null_literal && p.lit().sign() == is_true) {
            p.negate();
        }
        
        SASSERT(p.lit() == null_literal || value(p.lit()) == l_true);
        unsigned sz = p.size(), bound = p.k();

        // put the non-false literals into the head.
        unsigned slack = 0, slack1 = 0, num_watch = 0, j = 0;
        for (unsigned i = 0; i < sz; ++i) {
            if (value(p[i].second) != l_false) {
                if (j != i) {
                    p.swap(i, j);
                }
                if (slack <= bound) {
                    slack += p[j].first;
                    ++num_watch;
                }
                else {
                    slack1 += p[j].first;
                }
                ++j;
            }
        }
        BADLOG(verbose_stream() << "watch " << num_watch << " out of " << sz << "\n");

        DEBUG_CODE(
            bool is_false = false;
            for (unsigned k = 0; k < sz; ++k) {
                SASSERT(!is_false || value(p[k].second) == l_false);
                SASSERT(k < j == (value(p[k].second) != l_false));
                is_false = value(p[k].second) == l_false;
            });

        if (slack < bound) {
            literal lit = p[j].second;
            SASSERT(value(lit) == l_false);
            for (unsigned i = j + 1; i < sz; ++i) {
                if (lvl(lit) < lvl(p[i].second)) {
                    lit = p[i].second;
                }
            }
            set_conflict(p, lit);
            return false;
        }
        else {            
            for (unsigned i = 0; i < num_watch; ++i) {
                watch_literal(p[i], p);
            }
            p.set_slack(slack);
            p.set_num_watch(num_watch);

            SASSERT(validate_watch(p));

            TRACE("sat", display(tout << "init watch: ", p, true););

            // slack is tight:
            if (slack + slack1 == bound) {
                SASSERT(slack1 == 0);
                SASSERT(j == num_watch);                
                for (unsigned i = 0; i < j; ++i) {
                    assign(p, p[i].second);
                }
            }
            return true;
        }
    }

    /*
      Chai Kuhlmann:
      Lw - set of watched literals
      Lu - set of unwatched literals that are not false
      
      Lw = Lw \ { alit }
      Sw -= value
      a_max = max { a | l in Lw u Lu, l = undef }
      while (Sw < k + a_max & Lu != 0) {
        a_s = max { a | l in Lu }
        Sw += a_s
        Lw = Lw u {l_s}
        Lu = Lu \ {l_s}
      }
      if (Sw < k) return conflict
      for (li in Lw | Sw < k + ai) 
         assign li
      return no-conflict

     a_max index: index of non-false literal with maximal weight.
    */

    void ba_solver::add_index(pb& p, unsigned index, literal lit) {
        if (value(lit) == l_undef) {
            m_pb_undef.push_back(index);
            if (p[index].first > m_a_max) {
                m_a_max = p[index].first;
            }
        }
    }    
    
    /*
      \brief propagate assignment to alit in constraint p.
      
      TBD: 
      - consider reordering literals in watch list so that the search for watched literal takes average shorter time.
      - combine with caching literals that are assigned to 'true' to a cold store where they are not being revisited.
        Since 'true' literals may be unassigned (unless they are assigned at level 0) the cache has to be backtrack
        friendly (and the overhead of backtracking has to be taken into account).
     */
    lbool ba_solver::add_assign(pb& p, literal alit) {
        
        BADLOG(display(verbose_stream() << "assign: " << alit << " watch: " << p.num_watch() << " size: " << p.size(), p, true));
        TRACE("sat", display(tout << "assign: " << alit << "\n", p, true););
        SASSERT(!inconsistent());
        unsigned sz = p.size();
        unsigned bound = p.k();
        unsigned num_watch = p.num_watch();
        unsigned slack = p.slack();
        SASSERT(value(alit) == l_false);
        SASSERT(p.lit() == null_literal || value(p.lit()) == l_true);
        SASSERT(num_watch <= sz);
        SASSERT(num_watch > 0);
        unsigned index = 0;
        m_a_max = 0;
        m_pb_undef.reset();
        for (; index < num_watch; ++index) {
            literal lit = p[index].second;
            if (lit == alit) {
                break;
            }
            add_index(p, index, lit);
        }
        if (index == num_watch || num_watch == 0) {
            _bad_id = p.id();
            BADLOG(
                verbose_stream() << "BAD: " << p.id() << "\n";
                display(verbose_stream(), p, true);
                verbose_stream() << "alit: " << alit << "\n";
                verbose_stream() << "num watch " << num_watch << "\n");
            exit(0);
            return l_undef;
        }

        SASSERT(validate_watch(p));
        
        SASSERT(index < num_watch);
        unsigned index1 = index + 1;
        for (; m_a_max == 0 && index1 < num_watch; ++index1) {
            add_index(p, index1, p[index1].second);
        }
        
        unsigned val = p[index].first;
        SASSERT(value(p[index].second) == l_false);
        SASSERT(val <= slack);
        slack -= val;

        // find literals to swap with:            
        for (unsigned j = num_watch; j < sz && slack < bound + m_a_max; ++j) {
            literal lit = p[j].second;
            if (value(lit) != l_false) {
                slack += p[j].first;
                SASSERT(!is_watched(p[j].second, p));
                watch_literal(p[j], p);
                p.swap(num_watch, j);
                add_index(p, num_watch, lit);
                BADLOG(verbose_stream() << "add watch: " << lit << " num watch: " << num_watch << "\n");                
                ++num_watch;
            }
        }
        
        SASSERT(!inconsistent());
        DEBUG_CODE(for (auto idx : m_pb_undef) { SASSERT(value(p[idx].second) == l_undef); });
        
        if (slack < bound) {
            // maintain watching the literal
            slack += val;
            p.set_slack(slack);
            p.set_num_watch(num_watch);
            validate_watch(p);
            BADLOG(display(verbose_stream() << "conflict: " << alit << " watch: " << p.num_watch() << " size: " << p.size(), p, true));            
            SASSERT(bound <= slack);
            TRACE("sat", tout << "conflict " << alit << "\n";);
            set_conflict(p, alit);
            return l_false;
        }

        if (num_watch == 1) { _bad_id = p.id(); }

        BADLOG(verbose_stream() << "size: " << p.size() << " index: " << index << " num watch: " << num_watch << "\n");

        // swap out the watched literal.
        --num_watch;
        SASSERT(num_watch > 0);
        p.set_slack(slack);
        p.set_num_watch(num_watch);
        p.swap(num_watch, index);


        // 
        // slack >= bound, but slack - w(l) < bound 
        // l must be true.
        // 
        if (slack < bound + m_a_max) {            
            TRACE("sat", tout << p; for(auto j : m_pb_undef) tout << j << "\n";);
            for (unsigned index1 : m_pb_undef) {
                if (index1 == num_watch) {
                    index1 = index;
                }
                wliteral wl = p[index1];
                literal lit = wl.second;
                SASSERT(value(lit) == l_undef);
                BADLOG(verbose_stream() << "Assign " << lit << "\n");                
                if (slack < bound + wl.first) {
                    assign(p, lit);
                }
            }
        }

        TRACE("sat", display(tout << "assign: " << alit << "\n", p, true););

        BADLOG(verbose_stream() << "unwatch " << alit << " watch: " << p.num_watch() << " size: " << p.size() << " slack: " << p.slack() << " " << inconsistent() << "\n");

        return l_undef;
    }

    void ba_solver::watch_literal(wliteral l, pb& p) {
        watch_literal(l.second, p);
    }

    void ba_solver::clear_watch(pb& p) {
        for (unsigned i = 0; i < p.num_watch(); ++i) {
            unwatch_literal(p[i].second, p);          
        }  
        p.set_num_watch(0);
    }

    void ba_solver::recompile(pb& p) {
        // IF_VERBOSE(2, verbose_stream() << "re: " << p << "\n";);
        SASSERT(p.num_watch() == 0);
        m_weights.resize(2*s().num_vars(), 0);
        for (wliteral wl : p) {
            m_weights[wl.second.index()] += wl.first;
        }
        unsigned k = p.k();
        unsigned sz = p.size();
        bool all_units = true;
        for (unsigned i = 0; i < sz && 0 < k; ++i) {
            literal l = p[i].second;
            unsigned w1 = m_weights[l.index()];
            unsigned w2 = m_weights[(~l).index()];
            if (w1 == 0 || w1 < w2) {
                p.swap(i, sz - 1);
                --sz;
                --i;
            }
            else if (k <= w2) {
                k = 0;
                break;
            }
            else {
                SASSERT(w2 <= w1 && w2 < k);
                k -= w2;
                w1 -= w2;
                m_weights[l.index()] = 0;
                m_weights[(~l).index()] = 0;        
                if (w1 == 0) {
                    p.swap(i, sz - 1);
                    --sz;
                    --i;
                }    
                else {
                    p[i] = wliteral(w1, l);            
                    all_units &= w1 == 1;
                }
            }
        }
        // clear weights
        for (wliteral wl : p) {
            m_weights[wl.second.index()] = 0;
            m_weights[(~wl.second).index()] = 0;
        }

        if (k == 0) {
            if (p.lit() != null_literal) {
                s().assign(p.lit(), justification());
            }
            remove_constraint(p);
            return;
        }

        if (k == 1 && p.lit() == null_literal) {
            literal_vector lits(p.literals());
            s().mk_clause(lits.size(), lits.c_ptr(), p.learned());
            remove_constraint(p);
            return;
        }

        if (all_units) {
            literal_vector lits(p.literals());
            add_at_least(p.lit(), lits, k, p.learned());
            remove_constraint(p);
            return;
        }

        p.set_size(sz);
        p.set_k(k);
        SASSERT(p.well_formed());

        // this could become a cardinality constraint by now.
        if (p.lit() == null_literal || value(p.lit()) == l_true) {
            init_watch(p, true);
        }
    }

    void ba_solver::simplify2(pb& p) {
        return;

        if (p.is_cardinality()) {
            literal_vector lits(p.literals());
            unsigned k = (p.k() + p[0].first - 1) / p[0].first;
            add_at_least(p.lit(), lits, k, p.learned());
            remove_constraint(p);
        }
        else if (p.lit() == null_literal) {
            for (wliteral wl : p) {
                if (p.k() > p.max_sum() - wl.first) {
                    TRACE("sat", 
                          tout << "unit literal " << wl.second << "\n";
                          display(tout, p, true););
                    
                    s().assign(wl.second, justification());
                }
            }
        }
    }

    void ba_solver::display(std::ostream& out, pb const& p, bool values) const {
        if (p.lit() != null_literal) out << p.lit() << " == ";
        if (p.lit() != null_literal && values) {
            out << "[watch: " << p.num_watch() << ", slack: " << p.slack() << "]";
            out << "@(" << value(p.lit());
            if (value(p.lit()) != l_undef) {
                out << ":" << lvl(p.lit());
            }
            out << "): ";
        }
        for (wliteral wl : p) {
            literal l = wl.second;
            unsigned w = wl.first;
            if (w > 1) out << w << " * ";
            out << l;
            if (values) {
                out << "@(" << value(l);
                if (value(l) != l_undef) {
                    out << ":" << lvl(l);
                }
                out << ") ";
            }
            else {
                out << " ";
            }
        }
        out << ">= " << p.k()  << "\n";
    }


    // --------------------
    // xor:

    void ba_solver::clear_watch(xor& x) {
        unwatch_literal(x[0], x);
        unwatch_literal(x[1], x);         
        unwatch_literal(~x[0], x);
        unwatch_literal(~x[1], x);         
    }

    bool ba_solver::parity(xor const& x, unsigned offset) const {
        bool odd = false;
        unsigned sz = x.size();
        for (unsigned i = offset; i < sz; ++i) {
            SASSERT(value(x[i]) != l_undef);
            if (value(x[i]) == l_true) {
                odd = !odd;
            }
        }
        return odd;
    }

    bool ba_solver::init_watch(xor& x, bool is_true) {
        clear_watch(x);
        if (x.lit() != null_literal && x.lit().sign() == is_true) {
            x.negate();
        }
        TRACE("sat", display(tout, x, true););
        unsigned sz = x.size();
        unsigned j = 0;
        for (unsigned i = 0; i < sz && j < 2; ++i) {
            if (value(x[i]) == l_undef) {
                x.swap(i, j);
                ++j;
            }
        }
        switch (j) {
        case 0: 
            if (!parity(x, 0)) {
                unsigned l = lvl(x[0]);
                j = 1;
                for (unsigned i = 1; i < sz; ++i) {
                    if (lvl(x[i]) > l) {
                        j = i;
                        l = lvl(x[i]);
                    } 
                }
                SASSERT(x.lit() == null_literal || value(x.lit()) == l_true);
                set_conflict(x, x[j]);
            }
            return false;
        case 1: 
            SASSERT(x.lit() == null_literal || value(x.lit()) == l_true);
            assign(x, parity(x, 1) ? ~x[0] : x[0]);
            return false;
        default: 
            SASSERT(j == 2);
            watch_literal(x[0], x);
            watch_literal(x[1], x);
            watch_literal(~x[0], x);
            watch_literal(~x[1], x);
            return true;
        }
    }


    lbool ba_solver::add_assign(xor& x, literal alit) {
        // literal is assigned     
        unsigned sz = x.size();
        TRACE("sat", tout << "assign: " << x.lit() << ": " << ~alit << "@" << lvl(~alit) << "\n";);

        SASSERT(x.lit() == null_literal || value(x.lit()) == l_true);
        SASSERT(value(alit) != l_undef);
        unsigned index = 0;
        for (; index <= 2; ++index) {
            if (x[index].var() == alit.var()) break;
        }
        if (index == 2) {
            // literal is no longer watched.
            UNREACHABLE();
            return l_undef;
        }
        SASSERT(x[index].var() == alit.var());
        
        // find a literal to swap with:
        for (unsigned i = 2; i < sz; ++i) {
            literal lit2 = x[i];
            if (value(lit2) == l_undef) {
                x.swap(index, i);
                watch_literal(lit2, x);
                return l_undef;
            }
        }
        if (index == 0) {
            x.swap(0, 1);
        }
        // alit resides at index 1.
        SASSERT(x[1].var() == alit.var());        
        if (value(x[0]) == l_undef) {
            bool p = parity(x, 1);
            assign(x, p ? ~x[0] : x[0]);            
        }
        else if (!parity(x, 0)) {
            set_conflict(x, ~x[1]);
        }      
        return inconsistent() ? l_false : l_true;  
    }

    // ---------------------------
    // conflict resolution
    
    void ba_solver::normalize_active_coeffs() {
        reset_active_var_set();
        unsigned i = 0, j = 0, sz = m_active_vars.size();
        for (; i < sz; ++i) {
            bool_var v = m_active_vars[i];
            if (!m_active_var_set.contains(v) && get_coeff(v) != 0) {
                m_active_var_set.insert(v);
                if (j != i) {
                    m_active_vars[j] = m_active_vars[i];
                }
                ++j;
            }
        }
        m_active_vars.shrink(j);
    }

    void ba_solver::inc_coeff(literal l, unsigned offset) {
        SASSERT(offset > 0);
        bool_var v = l.var();
        SASSERT(v != null_bool_var);
        m_coeffs.reserve(v + 1, 0);

        int64 coeff0 = m_coeffs[v];
        if (coeff0 == 0) {
            m_active_vars.push_back(v);
        }
        
        int64 loffset = static_cast<int64>(offset);
        int64 inc = l.sign() ? -loffset : loffset;
        int64 coeff1 = inc + coeff0;
        m_coeffs[v] = coeff1;
        if (coeff1 > INT_MAX || coeff1 < INT_MIN) {
            m_overflow = true;
            return;
        }

        if (coeff0 > 0 && inc < 0) {
            inc_bound(std::max(0LL, coeff1) - coeff0);
        }
        else if (coeff0 < 0 && inc > 0) {
            inc_bound(coeff0 - std::min(0LL, coeff1));
        }

        // reduce coefficient to be no larger than bound.
        if (coeff1 > static_cast<int64>(m_bound)) {
            m_coeffs[v] = m_bound;
        }
        else if (coeff1 < 0 && -coeff1 > static_cast<int64>(m_bound)) {
            m_coeffs[v] = m_bound;
        }
    }

    int64 ba_solver::get_coeff(bool_var v) const {
        return m_coeffs.get(v, 0);
    }

    unsigned ba_solver::get_abs_coeff(bool_var v) const {
        int64 c = get_coeff(v);
        if (c < INT_MIN+1 || c > UINT_MAX) {
            m_overflow = true;
            return UINT_MAX;
        }
        return static_cast<unsigned>(abs(c));
    }

    int ba_solver::get_int_coeff(bool_var v) const {
        int64 c = m_coeffs.get(v, 0);
        if (c < INT_MIN || c > INT_MAX) {
            m_overflow = true;
            return 0;
        }
        return static_cast<int>(c);
    }

    void ba_solver::inc_bound(int64 i) {
        if (i < INT_MIN || i > INT_MAX) {
            m_overflow = true;
            return;
        }
        int64 new_bound = m_bound;
        new_bound += i;
        if (new_bound < 0) {
            // std::cout << "new negative bound " << new_bound << "\n";
            m_overflow = true;
        }
        else if (new_bound > UINT_MAX) {
            m_overflow = true;
        }
        else {
            m_bound = static_cast<unsigned>(new_bound);
        }
    }

    void ba_solver::reset_coeffs() {
        for (unsigned i = 0; i < m_active_vars.size(); ++i) {
            m_coeffs[m_active_vars[i]] = 0;
        }
        m_active_vars.reset();
    }

    static bool _debug_conflict = false;
    static literal _debug_consequent = null_literal;
    static unsigned_vector _debug_var2position;

    lbool ba_solver::resolve_conflict() {        
        if (0 == m_num_propagations_since_pop) {
            return l_undef;
        }
        m_overflow = false;
        reset_coeffs();
        m_num_marks = 0;
        m_bound = 0;
        literal consequent = s().m_not_l;
        justification js = s().m_conflict;
        TRACE("sat", tout << consequent << " " << js << "\n"; s().display(tout););
        m_conflict_lvl = s().get_max_lvl(consequent, js);
        if (consequent != null_literal) {
            consequent.neg();
            process_antecedent(consequent, 1);
        }
        literal_vector const& lits = s().m_trail;
        unsigned idx = lits.size() - 1;
        unsigned offset = 1;
        DEBUG_CODE(active2pb(m_A););

        unsigned init_marks = m_num_marks;

        do {

            if (m_overflow || offset > (1 << 12)) {
                IF_VERBOSE(20, verbose_stream() << "offset: " << offset << "\n";
                           active2pb(m_A);
                           display(verbose_stream(), m_A););
                goto bail_out;
            }

            if (offset == 0) {
                goto process_next_resolvent;            
            }

            TRACE("sat_verbose", display(tout, m_A););
            TRACE("sat", tout << "process consequent: " << consequent << ":\n"; s().display_justification(tout, js) << "\n";);
            SASSERT(offset > 0);

            // DEBUG_CODE(justification2pb(js, consequent, offset, m_B););
            
            if (_debug_conflict) {
                IF_VERBOSE(0, 
                           verbose_stream() << consequent << "\n";
                           s().display_justification(verbose_stream(), js);
                           verbose_stream() << "\n";);
                _debug_consequent = consequent;
            }
            switch(js.get_kind()) {
            case justification::NONE:
                SASSERT (consequent != null_literal);
                inc_bound(offset);
                break;
            case justification::BINARY:
                inc_bound(offset);
                SASSERT (consequent != null_literal);
                inc_coeff(consequent, offset);
                process_antecedent(js.get_literal(), offset);
                break;
            case justification::TERNARY:
                inc_bound(offset); 
                SASSERT (consequent != null_literal);
                inc_coeff(consequent, offset);
                process_antecedent(js.get_literal1(), offset);
                process_antecedent(js.get_literal2(), offset);
                break;
            case justification::CLAUSE: {
                inc_bound(offset); 
                clause & c = *(s().m_cls_allocator.get_clause(js.get_clause_offset()));
                unsigned i = 0;
                if (consequent != null_literal) {
                    inc_coeff(consequent, offset);
                    if (c[0] == consequent) {
                        i = 1;
                    }
                    else {
                        SASSERT(c[1] == consequent);
                        process_antecedent(c[0], offset);
                        i = 2;
                    }
                }
                unsigned sz = c.size();
                for (; i < sz; i++)
                    process_antecedent(c[i], offset);
                break;
            }
            case justification::EXT_JUSTIFICATION: {
                constraint& cnstr = index2constraint(js.get_ext_justification_idx());
                ++m_stats.m_num_resolves;
                switch (cnstr.tag()) {
                case card_t: {
                    card& c = cnstr.to_card();
                    inc_bound(static_cast<int64>(offset) * c.k());
                    process_card(c, offset);
                    break;
                }
                case pb_t: {
                    pb& p = cnstr.to_pb();
                    m_lemma.reset();
                    inc_bound(offset);
                    inc_coeff(consequent, offset);
                    get_antecedents(consequent, p, m_lemma);
                    TRACE("sat", display(tout, p, true); tout << m_lemma << "\n";);
                    if (_debug_conflict) {
                        verbose_stream() << consequent << " ";
                        verbose_stream() << "antecedents: " << m_lemma << "\n";
                    }
                    for (literal l : m_lemma) process_antecedent(~l, offset);
                    break;
                }
                case xor_t: {
                    // jus.push_back(js);
                    m_lemma.reset();
                    inc_bound(offset);
                    inc_coeff(consequent, offset);
                    get_xor_antecedents(consequent, idx, js, m_lemma);
                    for (literal l : m_lemma) process_antecedent(~l, offset);
                    break;
                }
                default:
                    UNREACHABLE();
                    break;
                }
                break;
            }
            default:
                UNREACHABLE();
                break;
            }
            
            SASSERT(validate_lemma());            

            DEBUG_CODE(
                active2pb(m_C);
                //SASSERT(validate_resolvent());
                m_A = m_C;);

            TRACE("sat", display(tout << "conflict:\n", m_A););

            cut();

        process_next_resolvent:
            
            // find the next marked variable in the assignment stack
            //
            bool_var v;
            while (true) {
                consequent = lits[idx];
                v = consequent.var();
                if (s().is_marked(v)) break;
                if (idx == 0) {
                    IF_VERBOSE(2, verbose_stream() << "did not find marked literal\n";);
                    goto bail_out;
                }
                SASSERT(idx > 0);
                --idx;
            }
            
            SASSERT(lvl(v) == m_conflict_lvl);
            s().reset_mark(v);
            --idx;
            TRACE("sat", tout << "Unmark: v" << v << "\n";);
            --m_num_marks;
            js = s().m_justification[v];
            offset = get_abs_coeff(v);
            if (offset > m_bound) {
                int64 bound64 = static_cast<int64>(m_bound);
                m_coeffs[v] = (get_coeff(v) < 0) ? -bound64 : bound64;
                offset = m_bound;
                DEBUG_CODE(active2pb(m_A););
            }
            SASSERT(value(consequent) == l_true);
        }        
        while (m_num_marks > 0);
        
        DEBUG_CODE(for (bool_var i = 0; i < static_cast<bool_var>(s().num_vars()); ++i) SASSERT(!s().is_marked(i)););
        SASSERT(validate_lemma());

        normalize_active_coeffs();

        if (!create_asserting_lemma()) {
            goto bail_out;
        }
        
        active2card();

        if (m_overflow) {
            goto bail_out;
        }

        SASSERT(validate_conflict(m_lemma, m_A));
        
        TRACE("sat", tout << m_lemma << "\n";);

        if (get_config().m_drat) {
            svector<drat::premise> ps; // TBD fill in
            drat_add(m_lemma, ps);
        }

        s().m_lemma.reset();
        s().m_lemma.append(m_lemma);
        for (unsigned i = 1; i < m_lemma.size(); ++i) {
            CTRACE("sat", s().is_marked(m_lemma[i].var()), tout << "marked: " << m_lemma[i] << "\n";);
            s().mark(m_lemma[i].var());
        }

        return l_true;

    bail_out:

        m_overflow = false;

        while (m_num_marks > 0 && idx >= 0) {
            bool_var v = lits[idx].var();
            if (s().is_marked(v)) {
                s().reset_mark(v);
                --m_num_marks;
            }
            if (idx == 0 && !_debug_conflict) {
                _debug_conflict = true;
                _debug_var2position.reserve(s().num_vars());
                for (unsigned i = 0; i < lits.size(); ++i) {
                    _debug_var2position[lits[i].var()] = i;
                }
                IF_VERBOSE(0, 
                           active2pb(m_A);
                           uint64 c = 0;
                           for (uint64 c1 : m_A.m_coeffs) c += c1;
                           std::cout << "sum of coefficients: " << c << "\n";
                           display(std::cout, m_A, true);
                           std::cout << "conflicting literal: " << s().m_not_l << "\n";);

                for (literal l : lits) {
                    if (s().is_marked(l.var())) {
                        IF_VERBOSE(0, verbose_stream() << "missing mark: " << l << "\n";);
                        s().reset_mark(l.var());
                    }
                }
                m_num_marks = 0;
                resolve_conflict();                
            }
            --idx;
        }
        return l_undef;
    }

    bool ba_solver::create_asserting_lemma() {

    adjust_conflict_level:
        int64 bound64 = m_bound;
        int64 slack = -bound64;
        for (bool_var v : m_active_vars) {
            slack += get_abs_coeff(v);
        }

        m_lemma.reset();        
        m_lemma.push_back(null_literal);
        unsigned num_skipped = 0;
        int64 asserting_coeff = 0;
        for (unsigned i = 0; 0 <= slack && i < m_active_vars.size(); ++i) { 
            bool_var v = m_active_vars[i];
            int64 coeff = get_coeff(v);
            lbool val = value(v);
            bool is_true = val == l_true;
            bool append = coeff != 0 && val != l_undef && (coeff < 0 == is_true);
            if (append) {
                literal lit(v, !is_true);
                unsigned acoeff = get_abs_coeff(v);
                if (lvl(lit) == m_conflict_lvl) {
                    if (m_lemma[0] == null_literal) {
                        asserting_coeff = abs(coeff);
                        slack -= asserting_coeff;
                        m_lemma[0] = ~lit;
                    }
                    else {
                        ++num_skipped;
                        if (asserting_coeff < abs(coeff)) {
                            m_lemma[0] = ~lit;
                            slack -= (abs(coeff) - asserting_coeff);
                            asserting_coeff = abs(coeff);
                        }
                    }
                }
                else {
                    slack -= abs(coeff);
                    m_lemma.push_back(~lit);
                }
            }
        }

        if (slack >= 0) {
            IF_VERBOSE(20, verbose_stream() << "(sat.card slack: " << slack << " skipped: " << num_skipped << ")\n";);
            return false;
        }

        
        if (m_lemma[0] == null_literal) {
            if (m_lemma.size() == 1) {
                s().set_conflict(justification());
                return false;
            }
            unsigned old_level = m_conflict_lvl;
            m_conflict_lvl = 0;
            for (unsigned i = 1; i < m_lemma.size(); ++i) {
                m_conflict_lvl = std::max(m_conflict_lvl, lvl(m_lemma[i]));
            }
            IF_VERBOSE(10, verbose_stream() << "(sat.backjump :new-level " << m_conflict_lvl << " :old-level " << old_level << ")\n";);
            goto adjust_conflict_level;
        }
        return true;
    }

    /*
      \brief compute a cut for current resolvent.
     */

    void ba_solver::cut() {

        // bypass cut if there is a unit coefficient
        for (bool_var v : m_active_vars) {
            if (1 == get_abs_coeff(v)) return;
        }

        unsigned g = 0;

        for (unsigned i = 0; g != 1 && i < m_active_vars.size(); ++i) {
            bool_var v = m_active_vars[i];
            unsigned coeff = get_abs_coeff(v);
            if (coeff == 0) {
                continue;
            }
            if (m_bound < coeff) {
                int64 bound64 = m_bound;
                if (get_coeff(v) > 0) {
                    m_coeffs[v] = bound64;
                }
                else {
                    m_coeffs[v] = -bound64;
                }
                coeff = m_bound;
            }
            SASSERT(0 < coeff && coeff <= m_bound);
            if (g == 0) {
                g = coeff;
            }
            else {
                g = u_gcd(g, coeff);
            }
        }

        if (g >= 2) {
            normalize_active_coeffs();
            for (bool_var v : m_active_vars) {
                m_coeffs[v] /= static_cast<int>(g);
            }
            m_bound = (m_bound + g - 1) / g;
            ++m_stats.m_num_cut;
        }        
    }

    void ba_solver::process_card(card& c, unsigned offset) {
        literal lit = c.lit();
        SASSERT(c.k() <= c.size());       
        SASSERT(lit == null_literal || value(lit) == l_true);
        SASSERT(0 < offset);
        for (unsigned i = c.k(); i < c.size(); ++i) {
            process_antecedent(c[i], offset);
        }
        for (unsigned i = 0; i < c.k(); ++i) {
            inc_coeff(c[i], offset);                        
        }
        if (lit != null_literal) {
            uint64 offset1 = static_cast<uint64>(offset) * c.k();
            if (offset1 > UINT_MAX) {
                m_overflow = true;
            }
            else {
                process_antecedent(~lit, static_cast<unsigned>(offset1));
            }
        }
    }

    void ba_solver::process_antecedent(literal l, unsigned offset) {
        SASSERT(value(l) == l_false);
        bool_var v = l.var();
        unsigned level = lvl(v);

        if (level > 0 && !s().is_marked(v) && level == m_conflict_lvl) {
            s().mark(v);
            TRACE("sat", tout << "Mark: v" << v << "\n";);
            ++m_num_marks;
            if (_debug_conflict && _debug_consequent != null_literal && _debug_var2position[_debug_consequent.var()] < _debug_var2position[l.var()]) {
                std::cout << "antecedent " << l << " is above consequent in stack\n";
            }
        }
        inc_coeff(l, offset);                
    }   

    literal ba_solver::get_asserting_literal(literal p) {
        if (get_abs_coeff(p.var()) != 0) {
            return p;
        }
        unsigned level = 0;        
        for (unsigned i = 0; i < m_active_vars.size(); ++i) { 
            bool_var v = m_active_vars[i];
            literal lit(v, get_coeff(v) < 0);
            if (value(lit) == l_false && lvl(lit) > level) {
                p = lit;
                level = lvl(lit);
            }
        }
        return p;        
    }

    ba_solver::ba_solver(): m_solver(0), m_lookahead(0), m_constraint_id(0) {        
        TRACE("sat", tout << this << "\n";);
    }

    ba_solver::~ba_solver() {
        m_stats.reset();
        for (constraint* c : m_constraints) {
            m_allocator.deallocate(c->obj_size(), c);
        }
        for (constraint* c : m_learned) {
            m_allocator.deallocate(c->obj_size(), c);
        }
    }

    void ba_solver::add_at_least(bool_var v, literal_vector const& lits, unsigned k) {
        literal lit = v == null_bool_var ? null_literal : literal(v, false);
        add_at_least(lit, lits, k, false);
    }

    ba_solver::constraint* ba_solver::add_at_least(literal lit, literal_vector const& lits, unsigned k, bool learned) {
        if (k == 1 && lit == null_literal) {
            literal_vector _lits(lits);
            s().mk_clause(_lits.size(), _lits.c_ptr(), learned);
            return 0;
        }
        void * mem = m_allocator.allocate(card::get_obj_size(lits.size()));
        card* c = new (mem) card(next_id(), lit, lits, k);
        c->set_learned(learned);
        add_constraint(c);
        return c;
    }

    void ba_solver::add_constraint(constraint* c) {
        if (c->learned()) {
            m_learned.push_back(c);
        }
        else {
            SASSERT(s().at_base_lvl());
            m_constraints.push_back(c);
        }
        literal lit = c->lit();
        if (c->learned() && !s().at_base_lvl()) {
            SASSERT(lit == null_literal);
            // gets initialized after backjump.
            m_constraint_to_reinit.push_back(c);
        }
        else if (lit == null_literal) {
            init_watch(*c, true);
        }
        else {
            s().set_external(lit.var());
            get_wlist(lit).push_back(c->index());
            get_wlist(~lit).push_back(c->index());
        }        
        SASSERT(c->well_formed());
    }


    bool ba_solver::init_watch(constraint& c, bool is_true) {
        if (inconsistent()) return false;
        switch (c.tag()) {
        case card_t: return init_watch(c.to_card(), is_true); 
        case pb_t: return init_watch(c.to_pb(), is_true); 
        case xor_t: return init_watch(c.to_xor(), is_true); 
        }
        UNREACHABLE();
        return false;
    }   

    lbool ba_solver::add_assign(constraint& c, literal l) {
        switch (c.tag()) {
        case card_t: return add_assign(c.to_card(), l); 
        case pb_t: return add_assign(c.to_pb(), l); 
        case xor_t: return add_assign(c.to_xor(), l); 
        }
        UNREACHABLE();
        return l_undef;
    }

    ba_solver::constraint* ba_solver::add_pb_ge(literal lit, svector<wliteral> const& wlits, unsigned k, bool learned) {
        bool units = true;
        for (wliteral wl : wlits) units &= wl.first == 1;
        if (k == 0 && lit == null_literal) {
            return 0;
        }
        if (units || k == 1) {
            literal_vector lits;
            for (wliteral wl : wlits) lits.push_back(wl.second);
            return add_at_least(lit, lits, k, learned);
        }
        void * mem = m_allocator.allocate(pb::get_obj_size(wlits.size()));
        pb* p = new (mem) pb(next_id(), lit, wlits, k);
        p->set_learned(learned);
        add_constraint(p);
        return p;
    }

    void ba_solver::add_pb_ge(bool_var v, svector<wliteral> const& wlits, unsigned k) {
        literal lit = v == null_bool_var ? null_literal : literal(v, false);
        add_pb_ge(lit, wlits, k, false);
    }

    void ba_solver::add_xor(bool_var v, literal_vector const& lits) {
        add_xor(literal(v, false), lits, false);
    }

    ba_solver::constraint* ba_solver::add_xor(literal lit, literal_vector const& lits, bool learned) {
        void * mem = m_allocator.allocate(xor::get_obj_size(lits.size()));
        xor* x = new (mem) xor(next_id(), lit, lits);
        x->set_learned(learned);
        add_constraint(x);
        for (literal l : lits) s().set_external(l.var()); // TBD: determine if goal2sat does this.
        return x;
    }

    /*
      \brief return true to keep watching literal.
    */
    bool ba_solver::propagate(literal l, ext_constraint_idx idx) {
        SASSERT(value(l) == l_true);
        TRACE("sat", tout << l << " " << idx << "\n";);
        constraint& c = index2constraint(idx);
        if (c.lit() != null_literal && l.var() == c.lit().var()) {
            init_watch(c, !l.sign());
            return true;
        }
        else if (c.lit() != null_literal && value(c.lit()) != l_true) {
            return true;
        }
        else {
            return l_undef != add_assign(c, ~l);
        }
    }

    double ba_solver::get_reward(card const& c, literal_occs_fun& literal_occs) const {
        unsigned k = c.k(), slack = 0;
        double to_add = 0;
        for (literal l : c) {
            switch (value(l)) {
            case l_true:  --k; if (k == 0) return 0; break;
            case l_undef: to_add += literal_occs(l); ++slack; break;
            case l_false: break;
            }
        }
        if (k >= slack) return 1;
        return pow(0.5, slack - k + 1) * to_add;
    }

    double ba_solver::get_reward(pb const& c, literal_occs_fun& occs) const {
        unsigned k = c.k(), slack = 0;
        double to_add = 0;
        double undefs = 0;
        for (wliteral wl : c) {
            literal l = wl.second;
            unsigned w = wl.first;
            switch (value(l)) {
            case l_true:  if (k <= w) return 0; k -= w; break;
            case l_undef: to_add += occs(l); ++undefs; slack += w; break; // TBD multiplier factor on this
            case l_false: break;
            }
        }
        if (k >= slack || 0 == undefs) return 0;
        double avg = slack / undefs;
        return pow(0.5, (slack - k + 1)/avg) * to_add;
    }

    double ba_solver::get_reward(literal l, ext_justification_idx idx, literal_occs_fun& occs) const {
        constraint const& c = index2constraint(idx);
        unsigned sz = c.size();
        switch (c.tag()) {
        case card_t: return get_reward(c.to_card(), occs);
        case pb_t: return get_reward(c.to_pb(), occs); 
        case xor_t: return 0;
        default: UNREACHABLE(); return 0;
        }
    }



    void ba_solver::ensure_parity_size(bool_var v) {
        if (m_parity_marks.size() <= static_cast<unsigned>(v)) {
            m_parity_marks.resize(static_cast<unsigned>(v) + 1, 0);
        }
    }
    
    unsigned ba_solver::get_parity(bool_var v) {
        return m_parity_marks.get(v, 0);
    }

    void ba_solver::inc_parity(bool_var v) {
        ensure_parity_size(v);
        m_parity_marks[v]++;
    }

    void ba_solver::reset_parity(bool_var v) {
        ensure_parity_size(v);
        m_parity_marks[v] = 0;
    }
    
    /**
       \brief perform parity resolution on xor premises.
       The idea is to collect premises based on xor resolvents. 
       Variables that are repeated an even number of times cancel out.
     */
    void ba_solver::get_xor_antecedents(literal l, unsigned index, justification js, literal_vector& r) {
        unsigned level = lvl(l);
        bool_var v = l.var();
        SASSERT(js.get_kind() == justification::EXT_JUSTIFICATION);
        TRACE("sat", tout << l << ": " << js << "\n"; tout << s().m_trail << "\n";);

        unsigned num_marks = 0;
        unsigned count = 0;
        while (true) {
            ++count;
            if (js.get_kind() == justification::EXT_JUSTIFICATION) {
                constraint& c = index2constraint(js.get_ext_justification_idx());
                if (!c.is_xor()) {
                    r.push_back(l);
                }
                else {
                    xor& x = c.to_xor();
                    if (x.lit() != null_literal && lvl(x.lit()) > 0) r.push_back(x.lit());
                    if (x[1].var() == l.var()) {
                        x.swap(0, 1);
                    }
                    SASSERT(x[0].var() == l.var());
                    for (unsigned i = 1; i < x.size(); ++i) {
                        literal lit(value(x[i]) == l_true ? x[i] : ~x[i]);
                        inc_parity(lit.var());
                        if (true || lvl(lit) == level) {
                            ++num_marks;
                        }
                        else {
                            m_parity_trail.push_back(lit);
                        }
                    }
                }
            }
            else {
                r.push_back(l);
            }
            while (num_marks > 0) {
                l = s().m_trail[index];
                v = l.var();
                unsigned n = get_parity(v);
                if (n > 0) {
                    reset_parity(v);
                    if (n % 2 == 1) {
                        break;
                    }
                    --num_marks;
                }
                --index;
            }
            if (num_marks == 0) {
                break;
            }
            --index;
            --num_marks;
            js = s().m_justification[v];
        }

        // now walk the defined literals 

        for (unsigned i = 0; i < m_parity_trail.size(); ++i) {
            literal lit = m_parity_trail[i];
            if (get_parity(lit.var()) % 2 == 1) {
                r.push_back(lit);
            }
            else {
                // IF_VERBOSE(2, verbose_stream() << "skip even parity: " << lit << "\n";);
            }
            reset_parity(lit.var());
        }
        m_parity_trail.reset();
        TRACE("sat", tout << r << "\n";);
    }

    /**
       \brief retrieve a sufficient set of literals from p that imply l.
       
       Find partition: 

         - Ax + coeff*l + B*y >= k
         - all literals in x are false.
         - B < k

       Then x is an explanation for l

     */
    void ba_solver::get_antecedents(literal l, pb const& p, literal_vector& r) {
        TRACE("sat", display(tout, p, true););
        SASSERT(p.lit() == null_literal || value(p.lit()) == l_true);

        if (p.lit() != null_literal) {
            r.push_back(p.lit());
        }

        unsigned k = p.k();

        if (_debug_conflict) {
            display(std::cout, p, true);
            std::cout << "literal: " << l << " value: " << value(l) << " num-watch: " << p.num_watch() << " slack: " << p.slack() << "\n";
        }

        if (value(l) == l_false) {
            // The literal comes from a conflict.
            // it is forced true, but assigned to false.
            unsigned slack = 0;
            for (wliteral wl : p) {
                if (value(wl.second) != l_false) {
                    slack += wl.first;
                }
            }
            SASSERT(slack < k);
            for (wliteral wl : p) {
                literal lit = wl.second;
                if (lit != l && value(lit) == l_false) {
                    unsigned w = wl.first;
                    if (slack + w < k) {
                        slack += w;
                    }
                    else {
                        r.push_back(~lit);
                    }
                } 
            }
        }
        else {
            unsigned coeff = 0, j = 0;
            for (; j < p.size(); ++j) {
                if (p[j].second == l) {
                    coeff = p[j].first;
                    break;
                }
            }
            
            ++j;
            if (j < p.num_watch()) {
                j = p.num_watch();
            }
            CTRACE("sat", coeff == 0, display(tout << l << " coeff: " << coeff << "\n", p, true);); 
            
            if (_debug_conflict) {
                std::cout << "coeff " << coeff << "\n";
            }

            SASSERT(coeff > 0);
            unsigned slack = p.slack() - coeff;
            j = std::max(j + 1, p.num_watch());
            
            for (; j < p.size(); ++j) {
                literal lit = p[j].second;
                unsigned w = p[j].first;
                SASSERT(l_false == value(lit));
                if (slack + w < k) {
                    slack += w;
                }
                else {
                    r.push_back(~lit); 
                }
            }
        }
        SASSERT(validate_unit_propagation(p, r, l));
    }

    bool ba_solver::is_extended_binary(ext_justification_idx idx, literal_vector & r) {
        constraint const& c = index2constraint(idx);
        switch (c.tag()) {
        case card_t: {
            card const& ca = c.to_card();
            if (ca.size() == ca.k() + 1 && ca.lit() == null_literal) {
                r.reset();
                for (literal l : ca) r.push_back(l);
                return true;
            }
            else {
                return false;
            }
        }
        default:
            return false;
        }
    }

    void ba_solver::simplify(xor& x) {
        // no-op
    }

    void ba_solver::get_antecedents(literal l, card const& c, literal_vector& r) {
        DEBUG_CODE(
            bool found = false;
            for (unsigned i = 0; !found && i < c.k(); ++i) {
                found = c[i] == l;
            }
            SASSERT(found););
        
        if (c.lit() != null_literal) r.push_back(c.lit());
        SASSERT(c.lit() == null_literal || value(c.lit()) == l_true);
        for (unsigned i = c.k(); i < c.size(); ++i) {
            SASSERT(value(c[i]) == l_false);
            r.push_back(~c[i]);
        }
    }

    void ba_solver::get_antecedents(literal l, xor const& x, literal_vector& r) {
        if (x.lit() != null_literal) r.push_back(x.lit());
        // TRACE("sat", display(tout << l << " ", x, true););
        SASSERT(x.lit() == null_literal || value(x.lit()) == l_true);
        SASSERT(x[0].var() == l.var() || x[1].var() == l.var());
        if (x[0].var() == l.var()) {
            SASSERT(value(x[1]) != l_undef);
            r.push_back(value(x[1]) == l_true ? x[1] : ~x[1]);                
        }
        else {
            SASSERT(value(x[0]) != l_undef);
            r.push_back(value(x[0]) == l_true ? x[0] : ~x[0]);                
        }
        for (unsigned i = 2; i < x.size(); ++i) {
            SASSERT(value(x[i]) != l_undef);
            r.push_back(value(x[i]) == l_true ? x[i] : ~x[i]);                
        }
    }

    // ----------------------------
    // constraint generic methods

    void ba_solver::get_antecedents(literal l, ext_justification_idx idx, literal_vector & r) {
        get_antecedents(l, index2constraint(idx), r);
    }

    bool ba_solver::is_watched(literal lit, constraint const& c) const {
        return get_wlist(~lit).contains(watched(c.index()));
    }
    
    void ba_solver::unwatch_literal(literal lit, constraint& c) {
        get_wlist(~lit).erase(watched(c.index()));
    }

    void ba_solver::watch_literal(literal lit, constraint& c) {
        get_wlist(~lit).push_back(watched(c.index()));
    }

    void ba_solver::get_antecedents(literal l, constraint const& c, literal_vector& r) {
        switch (c.tag()) {
        case card_t: get_antecedents(l, c.to_card(), r); break;
        case pb_t: get_antecedents(l, c.to_pb(), r); break;
        case xor_t: get_antecedents(l, c.to_xor(), r); break;
        default: UNREACHABLE(); break;
        }
    }

    void ba_solver::nullify_tracking_literal(constraint& c) {
        if (c.lit() != null_literal) {
            unwatch_literal(c.lit(), c);
            unwatch_literal(~c.lit(), c);
            c.nullify_literal();
        }
    }

    void ba_solver::clear_watch(constraint& c) {
        switch (c.tag()) {
        case card_t:
            clear_watch(c.to_card());
            break;
        case pb_t:
            clear_watch(c.to_pb());
            break;
        case xor_t:
            clear_watch(c.to_xor());
            break;
        default:
            UNREACHABLE();
        }
    }

    void ba_solver::remove_constraint(constraint& c) {
        nullify_tracking_literal(c);
        clear_watch(c);
        c.remove();
        m_constraint_removed = true;
    }

    // --------------------------------
    // validation

    bool ba_solver::validate_unit_propagation(constraint const& c, literal l) const {
        return true;
        switch (c.tag()) {
        case card_t:  return validate_unit_propagation(c.to_card(), l); 
        case pb_t: return validate_unit_propagation(c.to_pb(), l);
        case xor_t: return true;
        default: UNREACHABLE(); break;
        }
        return false;
    }

    bool ba_solver::validate_conflict(constraint const& c) const {
        return eval(c) == l_false;
    }

    lbool ba_solver::eval(constraint const& c) const {
        lbool v1 = c.lit() == null_literal ? l_true : value(c.lit());
        switch (c.tag()) {
        case card_t: return eval(v1, eval(c.to_card())); 
        case pb_t:   return eval(v1, eval(c.to_pb()));
        case xor_t:  return eval(v1, eval(c.to_xor()));
        default: UNREACHABLE(); break;
        }
        return l_undef;        
    }

    lbool ba_solver::eval(lbool a, lbool b) const {
        if (a == l_undef || b == l_undef) return l_undef;
        return (a == b) ? l_true : l_false;
    }

    lbool ba_solver::eval(card const& c) const {
        unsigned trues = 0, undefs = 0;
        for (literal l : c) {
            switch (value(l)) {
            case l_true: trues++; break;
            case l_undef: undefs++; break;
            default: break;
            }
        }
        if (trues + undefs < c.k()) return l_false;
        if (trues >= c.k()) return l_true;
        return l_undef;        
    }

    lbool ba_solver::eval(pb const& p) const {
        unsigned trues = 0, undefs = 0;
        for (wliteral wl : p) {
            switch (value(wl.second)) {
            case l_true: trues += wl.first; break;
            case l_undef: undefs += wl.first; break;
            default: break;
            }
        }
        if (trues + undefs < p.k()) return l_false;
        if (trues >= p.k()) return l_true;
        return l_undef;        
    }

    lbool ba_solver::eval(xor const& x) const {
        bool odd = false;
        
        for (auto l : x) {
            switch (value(l)) {
            case l_true: odd = !odd; break;
            case l_false: break;
            default: return l_undef;
            }
        }
        return odd ? l_true : l_false;
    }

    bool ba_solver::validate() {
        if (!validate_watch_literals()) {
            return false;
        }
        for (constraint* c : m_constraints) {
            if (!validate_watched_constraint(*c)) 
                return false;
        }
        for (constraint* c : m_learned) {
            if (!validate_watched_constraint(*c)) 
                return false;
        }
        return true;
    }

    bool ba_solver::validate_watch_literals() const {
        for (unsigned v = 0; v < s().num_vars(); ++v) {
            literal lit(v, false);
            if (lvl(lit) == 0) continue;
            if (!validate_watch_literal(lit)) return false;
            if (!validate_watch_literal(~lit)) return false;
        }        
        return true;
    }

    bool ba_solver::validate_watch_literal(literal lit) const {
        if (lvl(lit) == 0) return true;
        for (auto const & w : get_wlist(lit)) {
            if (w.get_kind() == watched::EXT_CONSTRAINT) {
                constraint const& c = index2constraint(w.get_ext_constraint_idx());
                if (!c.is_watching(~lit) && lit.var() != c.lit().var()) {
                    IF_VERBOSE(0, display(verbose_stream() << lit << " " << lvl(lit) << " is not watched in " << c << "\n", c, true););
                    UNREACHABLE();
                    return false;
                }
            }
        }
        return true;
    }

    bool ba_solver::validate_watched_constraint(constraint const& c) const {
        if (c.is_pb() && !validate_watch(c.to_pb())) {
            return false;
        }
        if (c.lit() != null_literal && value(c.lit()) != l_true) return true;
        if (c.lit() != null_literal && lvl(c.lit()) != 0) {
            if (!is_watched(c.lit(), c) || !is_watched(~c.lit(), c)) {
                UNREACHABLE();
                return false;
            }
        }
        if (eval(c) == l_true) {
            return true;
        }
        literal_vector lits(c.literals());
        for (literal l : lits) {
            if (lvl(l) == 0) continue;
            bool found = is_watched(l, c);
            if (found != c.is_watching(l)) {

                IF_VERBOSE(0, 
                           verbose_stream() << "Discrepancy of watched literal: " << l << " id: " << c.id() 
                           << " clause: " << c << (found?" is watched, but shouldn't be":" not watched, but should be") << "\n";
                           display_watch_list(verbose_stream() << l << ": ", s().m_cls_allocator, get_wlist(l)) << "\n";
                           display_watch_list(verbose_stream() << ~l << ": ", s().m_cls_allocator, get_wlist(~l)) << "\n";
                           verbose_stream() << "value: " << value(l) << " level: " << lvl(l) << "\n";
                           display(verbose_stream(), c, true);
                           if (c.lit() != null_literal) verbose_stream() << value(c.lit()) << "\n";);

                UNREACHABLE();
                exit(1);
                return false;
            }
        }
        return true;
    }

    bool ba_solver::validate_watch(pb const& p) const {
        for (unsigned i = 0; i < p.size(); ++i) {
            literal l = p[i].second;
            if (lvl(l) != 0 && is_watched(l, p) != i < p.num_watch()) {
                UNREACHABLE();
                return false;
            }
        }
        return true;
    }

    
    /**
       \brief Lex on (glue, size)
    */
    struct constraint_glue_psm_lt {
        bool operator()(ba_solver::constraint const * c1, ba_solver::constraint const * c2) const {
            return 
                (c1->glue()  < c2->glue()) ||
                (c1->glue() == c2->glue() && 
                 (c1->psm() < c2->psm() || 
                  (c1->psm() == c2->psm() && c1->size() < c2->size())));
        }
    };

    void ba_solver::update_psm(constraint& c) const {
        unsigned r = 0;
        switch (c.tag()) {            
        case card_t: 
            for (literal l : c.to_card()) {                
                if (s().m_phase[l.var()] == (l.sign() ? NEG_PHASE : POS_PHASE)) ++r;
            }
            break;
        case pb_t:
            for (wliteral l : c.to_pb()) {                
                if (s().m_phase[l.second.var()] == (l.second.sign() ? NEG_PHASE : POS_PHASE)) ++r;
            }
            break;
        default:
            break;
        }
        c.set_psm(r);
    }

    void ba_solver::gc() {
        if (m_learned.size() >= 2 * m_constraints.size()) {
            for (auto & c : m_learned) update_psm(*c);
            std::stable_sort(m_learned.begin(), m_learned.end(), constraint_glue_psm_lt());
            gc_half("glue-psm");
            cleanup_constraints(m_learned, true);
        }
    }

    void ba_solver::gc_half(char const* st_name) {
        TRACE("sat", tout << "gc\n";);
        unsigned sz     = m_learned.size();
        unsigned new_sz = sz/2;
        unsigned removed = 0;
        for (unsigned i = new_sz; i < sz; i++) {
            constraint* c = m_learned[i];
            if (!m_constraint_to_reinit.contains(c)) {
                remove_constraint(*c);
                ++removed;
            }
        }
        m_stats.m_num_gc += removed;
        m_learned.shrink(new_sz);
        IF_VERBOSE(2, verbose_stream() << "(sat-gc :strategy " << st_name << " :deleted " << removed << ")\n";);

    }

    lbool ba_solver::add_assign(card& c, literal alit) {
        // literal is assigned to false.        
        unsigned sz = c.size();
        unsigned bound = c.k();
        TRACE("sat", tout << "assign: " << c.lit() << ": " << ~alit << "@" << lvl(~alit) << "\n";);

        SASSERT(0 < bound && bound <= sz);
        if (bound == sz) {
            set_conflict(c, alit);
            return l_false;
        }
        SASSERT(value(alit) == l_false);
        SASSERT(c.lit() == null_literal || value(c.lit()) == l_true);
        unsigned index = 0;
        for (index = 0; index <= bound; ++index) {
            if (c[index] == alit) {
                break;
            }
        }
        if (index == bound + 1) {
            // literal is no longer watched.
            return l_undef;
        }
        SASSERT(index <= bound);
        SASSERT(c[index] == alit);
        
        // find a literal to swap with:
        for (unsigned i = bound + 1; i < sz; ++i) {
            literal lit2 = c[i];
            if (value(lit2) != l_false) {
                c.swap(index, i);
                watch_literal(lit2, c);
                return l_undef;
            }
        }

        // conflict
        if (bound != index && value(c[bound]) == l_false) {
            TRACE("sat", tout << "conflict " << c[bound] << " " << alit << "\n";);
            set_conflict(c, alit);
            return l_false;
        }

        // TRACE("sat", tout << "no swap " << index << " " << alit << "\n";);
        // there are no literals to swap with,
        // prepare for unit propagation by swapping the false literal into 
        // position bound. Then literals in positions 0..bound-1 have to be
        // assigned l_true.
        if (index != bound) {
            c.swap(index, bound);
        }        
        for (unsigned i = 0; i < bound; ++i) {
            assign(c, c[i]);
        }

        if (c.learned() && c.glue() > 2) {
            unsigned glue;
            if (s().num_diff_false_levels_below(c.size(), c.begin(), c.glue()-1, glue)) {
                c.set_glue(glue);
            }
        }

        return inconsistent() ? l_false : l_true;
    }

    void ba_solver::asserted(literal l) {
    }


    check_result ba_solver::check() { return CR_DONE; }

    void ba_solver::push() {
        m_constraint_to_reinit_lim.push_back(m_constraint_to_reinit.size());
    }

    void ba_solver::pop(unsigned n) {        
        TRACE("sat_verbose", tout << "pop:" << n << "\n";);
        unsigned new_lim = m_constraint_to_reinit_lim.size() - n;
        m_constraint_to_reinit_last_sz = m_constraint_to_reinit_lim[new_lim];
        m_constraint_to_reinit_lim.shrink(new_lim);
        m_num_propagations_since_pop = 0;
    }

    void ba_solver::pop_reinit() {
        unsigned sz = m_constraint_to_reinit_last_sz;
        for (unsigned i = sz; i < m_constraint_to_reinit.size(); ++i) {
            constraint* c = m_constraint_to_reinit[i];
            if (!init_watch(*c, true) && !s().at_base_lvl()) {
                m_constraint_to_reinit[sz++] = c;
            }
        }
        m_constraint_to_reinit.shrink(sz);        
    }

    
    void ba_solver::simplify(constraint& c) {
        SASSERT(s().at_base_lvl());
        switch (c.tag()) {
        case card_t:
            simplify(c.to_card());
            break;
        case pb_t:
            simplify(c.to_pb());
            break;
        case xor_t:
            simplify(c.to_xor());
            break;
        default:
            UNREACHABLE();
        }                
    }

    void ba_solver::simplify() {
        if (!s().at_base_lvl()) s().pop_to_base_level();
        unsigned trail_sz;
        do {
            trail_sz = s().init_trail_size();
            m_simplify_change = false;
            m_clause_removed = false;
            m_constraint_removed = false;
            for (unsigned sz = m_constraints.size(), i = 0; i < sz; ++i) simplify(*m_constraints[i]);
            for (unsigned sz = m_learned.size(), i = 0; i < sz; ++i) simplify(*m_learned[i]);
            init_use_lists();
            remove_unused_defs();
            set_non_external();
            elim_pure();
            for (unsigned sz = m_constraints.size(), i = 0; i < sz; ++i) subsumption(*m_constraints[i]);
            for (unsigned sz = m_learned.size(), i = 0; i < sz; ++i) subsumption(*m_learned[i]);
            cleanup_clauses();
            cleanup_constraints();
        }        
        while (m_simplify_change || trail_sz < s().init_trail_size());

        IF_VERBOSE(1, verbose_stream() << "(ba.simplify "
                   << " :vars " << s().num_vars() - trail_sz 
                   << " :constraints " << m_constraints.size() 
                   << " :lemmas " << m_learned.size() 
                   << " :subsumes " << m_stats.m_num_bin_subsumes 
                   + m_stats.m_num_clause_subsumes
                   + m_stats.m_num_pb_subsumes
                   << " :gc " << m_stats.m_num_gc
                   << ")\n";);

        // mutex_reduction();
        // if (s().m_clauses.size() < 80000) lp_lookahead_reduction();

    }

    void ba_solver::mutex_reduction() {
        literal_vector lits;
        for (unsigned v = 0; v < s().num_vars(); ++v) {
            lits.push_back(literal(v, false));
            lits.push_back(literal(v, true));
        }
        vector<literal_vector> mutexes;
        s().find_mutexes(lits, mutexes);
        for (literal_vector& mux : mutexes) {
            if (mux.size() > 2) {
                IF_VERBOSE(1, verbose_stream() << "mux: " << mux << "\n";);
                for (unsigned i = 0; i < mux.size(); ++i) mux[i].neg(); 
                add_at_least(null_literal, mux, mux.size() - 1, false);
            }
        }
    }

    // ----------------------------------
    // lp based relaxation 

    void ba_solver::lp_add_var(int coeff, lp::var_index v, lhs_t& lhs, rational& rhs) {
        if (coeff < 0) {
            rhs += rational(coeff);
        }
        lhs.push_back(std::make_pair(rational(coeff), v));
    }

    void ba_solver::lp_add_clause(lp::lar_solver& s, svector<lp::var_index> const& vars, clause const& c) {
        lhs_t lhs;
        rational rhs;
        if (c.frozen()) return;
        rhs = rational::one();
        for (literal l : c) {
            lp_add_var(l.sign() ? -1 : 1, vars[l.var()], lhs, rhs);
        }
        s.add_constraint(lhs, lp::GE, rhs);
    }

    void ba_solver::lp_lookahead_reduction() {
        lp::lar_solver solver;
        solver.settings().set_message_ostream(&std::cout);
        solver.settings().set_debug_ostream(&std::cout);
        solver.settings().print_statistics = true;
        solver.settings().report_frequency = 1000;
        // solver.settings().simplex_strategy() = lp::simplex_strategy_enum::lu; - runs out of memory
        // TBD: set rlimit on the solver
        svector<lp::var_index> vars;
        for (unsigned i = 0; i < s().num_vars(); ++i) {
            lp::var_index v = solver.add_var(i, false);
            vars.push_back(v);
            solver.add_var_bound(v, lp::GE, rational::zero());
            solver.add_var_bound(v, lp::LE, rational::one());
            switch (value(v)) {
            case l_true: solver.add_var_bound(v, lp::GE, rational::one()); break;
            case l_false: solver.add_var_bound(v, lp::LE, rational::zero()); break;
            default: break;
            }
        }
        lhs_t lhs;
        rational rhs;
        for (clause* c : s().m_clauses) {            
            lp_add_clause(solver, vars, *c);
        }
        for (clause* c : s().m_learned) {            
            lp_add_clause(solver, vars, *c);
        }
        for (constraint const* c : m_constraints) {
            if (c->lit() != null_literal) continue; // ignore definitions for now.
            switch (c->tag()) {
            case card_t:
            case pb_t: {
                pb_base const& p = dynamic_cast<pb_base const&>(*c);
                rhs = rational(p.k());
                lhs.reset();
                for (unsigned i = 0; i < p.size(); ++i) {
                    literal l = p.get_lit(i);
                    int co = p.get_coeff(i);
                    lp_add_var(l.sign() ? -co : co, vars[l.var()], lhs, rhs);
                }
                solver.add_constraint(lhs, lp::GE, rhs);
                break;
            }
            default:
                // ignore xor
                break;
            }
        }
        std::cout << "lp solve\n";
        std::cout.flush();

        lp::lp_status st = solver.solve();
        if (st == lp::lp_status::INFEASIBLE) {
            std::cout << "infeasible\n";
            s().set_conflict(justification());
            return;
        }
        std::cout << "feasible\n";
        std::cout.flush();
        for (unsigned i = 0; i < s().num_vars(); ++i) {
            lp::var_index v = vars[i];
            if (value(v) != l_undef) continue;
            // TBD: take initial model into account to limit queries.
            std::cout << "solve v" << v << "\n";
            std::cout.flush();
            solver.push();
            solver.add_var_bound(v, lp::GE, rational::one());
            st = solver.solve();
            solver.pop(1);
            if (st == lp::lp_status::INFEASIBLE) {
                std::cout << "found unit: " << literal(v, true) << "\n";
                s().assign(literal(v, true), justification());
                solver.add_var_bound(v, lp::LE, rational::zero());
                continue;
            }

            solver.push();
            solver.add_var_bound(v, lp::LE, rational::zero());
            st = solver.solve();
            solver.pop(1);
            if (st == lp::lp_status::INFEASIBLE) {
                std::cout << "found unit: " << literal(v, false) << "\n";
                s().assign(literal(v, false), justification());
                solver.add_var_bound(v, lp::GE, rational::zero());
                continue;
            }
        }
    }

    // -------------------------------
    // set literals equivalent

    bool ba_solver::set_root(literal l, literal r) { 
        if (s().is_assumption(l.var())) {
            return false;
        }
        m_root_vars.reserve(s().num_vars(), false);
        for (unsigned i = m_roots.size(); i < 2 * s().num_vars(); ++i) {
            m_roots.push_back(to_literal(i));
        }
        m_roots[l.index()] = r;
        m_roots[(~l).index()] = ~r;
        m_root_vars[l.var()] = true;
        return true;
    }

    void ba_solver::flush_roots() {
        if (m_roots.empty()) return;

        // validate();

        m_visited.resize(s().num_vars()*2, false);
        m_constraint_removed = false;
        for (unsigned sz = m_constraints.size(), i = 0; i < sz; ++i) 
            flush_roots(*m_constraints[i]);
        for (unsigned sz = m_learned.size(), i = 0; i < sz; ++i) 
            flush_roots(*m_learned[i]);
        cleanup_constraints();

        // validate();
    }

    void ba_solver::recompile(constraint& c) {
        if (c.id() == _bad_id) {
            display(std::cout << "recompile\n", c, true);
        }
        switch (c.tag()) {
        case card_t:
            recompile(c.to_card());
            break;
        case pb_t:
            recompile(c.to_pb());
            break;
        case xor_t:
            NOT_IMPLEMENTED_YET();
            break;
        default:
            UNREACHABLE();
        }                
    }

    void ba_solver::recompile(card& c) {
        if (c.id() == _bad_id) std::cout << "recompile: " << c << "\n";
        // IF_VERBOSE(0, verbose_stream() << "re: " << c << "\n";);
        m_weights.resize(2*s().num_vars(), 0);
        for (literal l : c) {
            ++m_weights[l.index()];
        }
        unsigned k = c.k();
        bool all_units = true;
        unsigned sz = c.size();
        unsigned_vector coeffs;
        for (unsigned i = 0; i < sz && 0 < k; ++i) {
            literal l = c[i];
            unsigned w = m_weights[l.index()];
            unsigned w2 = m_weights[(~l).index()];
            if (w == 0 || w < w2) {
                c.swap(i, sz - 1);
                --sz;
                --i;
            }
            else if (k <= w2) {
                k = 0;
                break;
            }
            else {
                SASSERT(w2 <= w && w2 < k);
                k -= w2;
                w -= w2;
                m_weights[(~l).index()] = 0;        
                m_weights[l.index()] = 0;        
                if (w == 0) {
                    c.swap(i, sz - 1);
                    --sz;
                    --i;
                }    
                else {
                    all_units &= (w == 1);
                    coeffs.push_back(w);
                }
            }
        }
        // clear weights
        for (literal l : c) {
            m_weights[l.index()] = 0;
            m_weights[(~l).index()] = 0;
        }       

        if (k == 0) {
            remove_constraint(c);
            return;
        }

        if (k == 1) {
            literal_vector lits(c.size(), c.begin());
            s().mk_clause(lits.size(), lits.c_ptr(), c.learned());
            remove_constraint(c);
            return;
        }

        c.set_size(sz);
        c.set_k(k);        

        if (!all_units) {            
            TRACE("sat", tout << "replacing by pb: " << c << "\n";);
            m_wlits.reset();
            for (unsigned i = 0; i < sz; ++i) {
                m_wlits.push_back(wliteral(coeffs[i], c[i]));
            }
            literal root = c.lit();
            remove_constraint(c);
            add_pb_ge(root, m_wlits, k, c.learned());
        }
        else {
            if (c.lit() == null_literal || value(c.lit()) == l_true) {
                init_watch(c, true);
            }
            SASSERT(c.well_formed());
        }        
    }


    void ba_solver::split_root(constraint& c) {
        switch (c.tag()) {
        case card_t: split_root(c.to_card()); break;
        case pb_t: split_root(c.to_pb()); break;
        case xor_t: NOT_IMPLEMENTED_YET(); break;
        }
    }



    void ba_solver::flush_roots(constraint& c) {
        bool found = c.lit() != null_literal && m_root_vars[c.lit().var()];
        for (unsigned i = 0; !found && i < c.size(); ++i) {
            found = m_root_vars[c.get_lit(i).var()];
        }
        if (!found) return;
        clear_watch(c);
        
        // this could create duplicate literals
        for (unsigned i = 0; i < c.size(); ++i) {
            c.set_lit(i, m_roots[c.get_lit(i).index()]);            
        }

        literal root = c.lit();
        if (c.lit() != null_literal && m_roots[c.lit().index()] != c.lit()) {
            root = m_roots[c.lit().index()];
            nullify_tracking_literal(c);
            c.update_literal(root);
            get_wlist(root).push_back(c.index());
            get_wlist(~root).push_back(c.index());
        }

        bool found_dup = false;
        bool found_root = false;
        for (unsigned i = 0; i < c.size(); ++i) {
            literal l = c.get_lit(i);
            if (is_marked(l)) {
                found_dup = true;
                break;
            }
            else {
                mark_visited(l);
                mark_visited(~l);
            }
        }
        for (unsigned i = 0; i < c.size(); ++i) {
            literal l = c.get_lit(i);
            unmark_visited(l);
            unmark_visited(~l);
            found_root |= l.var() == root.var();
        }

        if (found_root) {
            split_root(c);
            c.negate();
            split_root(c);
            remove_constraint(c);
        }
        else if (found_dup) {
            recompile(c);
        }
        else {
            // review for potential incompleteness: if c.lit() == l_false, do propagations happen?
            if (c.lit() == null_literal || value(c.lit()) == l_true) {    
                init_watch(c, true);
            }
            SASSERT(c.well_formed());
        }
    }

    unsigned ba_solver::get_num_non_learned_bin(literal l) {
        return s().m_simplifier.get_num_non_learned_bin(l);
    }

    /*
      \brief garbage collection.
      This entails 
      - finding pure literals, 
      - setting literals that are not used in the extension to non-external.
      - subsumption
      - resolution 
      - blocked literals
     */
    void ba_solver::init_use_lists() {
        m_visited.resize(s().num_vars()*2, false);
        m_clause_use_list.init(s().num_vars());
        m_cnstr_use_list.reset();
        m_cnstr_use_list.resize(2*s().num_vars());
        for (clause* c : s().m_clauses) {
            if (!c->frozen()) 
                m_clause_use_list.insert(*c);
        }
        for (constraint* cp : m_constraints) {
            literal lit = cp->lit();
            if (lit != null_literal) {
                m_cnstr_use_list[lit.index()].push_back(cp);
                m_cnstr_use_list[(~lit).index()].push_back(cp);
            }
            switch (cp->tag()) {
            case card_t: {
                card& c = cp->to_card();
                for (literal l : c) {
                    m_cnstr_use_list[l.index()].push_back(&c);
                    if (lit != null_literal) m_cnstr_use_list[(~l).index()].push_back(&c);
                }  
                break;
            }
            case pb_t: {
                pb& p = cp->to_pb();
                for (wliteral wl : p) {
                    literal l = wl.second;
                    m_cnstr_use_list[l.index()].push_back(&p);
                    if (lit != null_literal) m_cnstr_use_list[(~l).index()].push_back(&p);
                }
                break;
            }
            case xor_t: {
                xor& x = cp->to_xor();
                for (literal l : x) {
                    m_cnstr_use_list[l.index()].push_back(&x);
                    m_cnstr_use_list[(~l).index()].push_back(&x);
                }             
                break;
            }
            }
        }
    }

    void ba_solver::remove_unused_defs() {
        // remove constraints where indicator literal isn't used.
        for (constraint* cp : m_constraints) {
            constraint& c = *cp;
            literal lit = c.lit();
            switch (c.tag()) {
            case card_t: 
            case pb_t: {
                if (lit != null_literal && 
                    use_count(lit) == 1 &&
                    use_count(~lit) == 1 &&
                    get_num_non_learned_bin(lit) == 0 && 
                    get_num_non_learned_bin(~lit) == 0) {
                    remove_constraint(c);
                }
                break;
            }
            default:
                break;
            }
        }
    }
    
    unsigned ba_solver::set_non_external() {
        // set variables to be non-external if they are not used in theory constraints.
        unsigned ext = 0;
        for (unsigned v = 0; v < s().num_vars(); ++v) {
            literal lit(v, false);
            if (s().is_external(v) && 
                m_cnstr_use_list[lit.index()].size() == 0 && 
                m_cnstr_use_list[(~lit).index()].size() == 0 && !s().is_assumption(v)) {
                s().set_non_external(v);
                ++ext;
            }            
        }
        // ensure that lemmas use only external variables.
        for (constraint* cp : m_learned) {
            constraint& c = *cp;
            if (c.was_removed()) continue;
            SASSERT(c.lit() == null_literal);
            for (unsigned i = 0; i < c.size(); ++i) {
                bool_var v = c.get_lit(i).var();
                if (s().was_eliminated(v)) {
                    remove_constraint(c);                    
                    break;
                }
                if (!s().is_external(v)) {
                    s().set_external(v);
                }
            }
        }
        IF_VERBOSE(10, verbose_stream() << "non-external variables converted: " << ext << "\n";);
        return ext;
    }

    bool ba_solver::elim_pure(literal lit) {
        if (value(lit) != l_undef) return false;
        if (!m_cnstr_use_list[lit.index()].empty() && use_count(~lit) == 0 && 
            get_num_non_learned_bin(~lit) == 0) {
            s().assign(lit, justification());
            return true;
        }
        return false;
    }

    unsigned ba_solver::elim_pure() {
        // eliminate pure literals
        unsigned pure_literals = 0;
        for (unsigned v = 0; v < s().num_vars(); ++v) {
            literal lit(v, false);
            if (value(v) != l_undef) continue;
            if (m_cnstr_use_list[lit.index()].empty() &&
                m_cnstr_use_list[(~lit).index()].empty()) continue;

            if (elim_pure(lit) || elim_pure(~lit)) {
                ++pure_literals;
            }
        }
        IF_VERBOSE(10, verbose_stream() << "pure literals converted: " << pure_literals << " " << inconsistent() << "\n";);
        return pure_literals;
    }

    void ba_solver::subsumption(constraint& cnstr) {
        if (cnstr.was_removed()) return;
        switch (cnstr.tag()) {
        case card_t: {
            card& c = cnstr.to_card();
            if (c.k() > 1) subsumption(c);
            break;
        }
        case pb_t: {
            pb& p = cnstr.to_pb();
            if (p.k() > 1) subsumption(p);
            break;
        }
        default:
            break;                
        }
    }

    void ba_solver::cleanup_clauses() {
        if (!m_clause_removed) return;
        // version in simplify first clears 
        // all watch literals, then reinserts them.
        // this ensures linear time cleanup.
        clause_vector::iterator it = s().m_clauses.begin();
        clause_vector::iterator end = s().m_clauses.end();
        clause_vector::iterator it2 = it;
        for (; it != end; ++it) {
            clause* c = *it;
            if (c->was_removed()) {
                s().detach_clause(*c);
                s().del_clause(*c);
            }
            else {
                if (it2 != it) {
                    *it2 = *it;
                }
                ++it2;
            }
        }
        s().m_clauses.set_end(it2);        
    }
    
    void ba_solver::cleanup_constraints() {
        if (!m_constraint_removed) return;
        cleanup_constraints(m_constraints, false);
        cleanup_constraints(m_learned, true);
        m_constraint_removed = false;
    }

    void ba_solver::cleanup_constraints(ptr_vector<constraint>& cs, bool learned) {
        ptr_vector<constraint>::iterator it = cs.begin();
        ptr_vector<constraint>::iterator it2 = it;
        ptr_vector<constraint>::iterator end = cs.end();
        for (; it != end; ++it) {
            constraint& c = *(*it);
            if (c.was_removed()) {
                m_allocator.deallocate(c.obj_size(), &c);
            }
            else if (learned && !c.learned()) {
                m_constraints.push_back(&c);
            }
            else {
                if (it != it2) {
                    *it2 = *it;
                }
                ++it2;
            }
        }
        cs.set_end(it2);
    }

    /*
      \brief subsumption between two cardinality constraints
      - A >= k       subsumes A + B >= k' for k' <= k
      - A + A' >= k  subsumes A + B >= k' for k' + |A'| <= k
      - A + lit >= k self subsumes A + ~lit + B >= k' into A + B >= k' for k' <= k
      - TBD: consider version that generalizes self-subsumption to more than one literal
        A + ~L + B >= k'   =>    A + B >= k'    if A + A' + L >= k and k' + |L| + |A'| <= k
     */
    bool ba_solver::subsumes(card& c1, card& c2, literal_vector & comp) {
        if (c2.lit() != null_literal) return false; 

        unsigned c2_exclusive = 0;
        unsigned common = 0;
        comp.reset();
        for (literal l : c2) {
            if (is_marked(l)) {
                ++common;
            }
            else if (is_marked(~l)) {
                comp.push_back(l);
            }
            else {
                ++c2_exclusive;
            }
        }

        unsigned c1_exclusive = c1.size() - common - comp.size();
        return c1_exclusive + c2.k() + comp.size() <= c1.k();
    }

    bool ba_solver::subsumes(card& c1, clause& c2, literal_vector & comp) {
        unsigned c2_exclusive = 0;
        unsigned common = 0;
        comp.reset();
        for (literal l : c2) {
            if (is_marked(l)) {
                ++common;
            }
            else if (is_marked(~l)) {
                comp.push_back(l);
            }
            else {
                ++c2_exclusive;
            }
        }

        if (!comp.empty()) {
            // self-subsumption is TBD.
            return false;
        }
        unsigned c1_exclusive = c1.size() - common - comp.size();
        return c1_exclusive + 1 <= c1.k();
    }

    /*
      \brief Ax >= k subsumes By >= k' if
      all coefficients in A are <= B and k >= k'
     */
    bool ba_solver::subsumes(pb const& p1, pb_base const& p2) {
        if (p1.k() < p2.k() || p1.size() > p2.size()) return false;
        unsigned num_sub = 0;
        for (unsigned i = 0; i < p2.size(); ++i) {
            literal l = p2.get_lit(i);
            if (is_marked(l) && m_weights[l.index()] <= p2.get_coeff(i)) {
                ++num_sub;
            }
        }
        return num_sub == p1.size();
    }

    void ba_solver::subsumes(pb& p1, literal lit) {
        for (constraint* c : m_cnstr_use_list[lit.index()]) {
            if (c == &p1 || c->was_removed()) continue;
            bool s = false;
            switch (c->tag()) {
            case card_t: 
                s = subsumes(p1, c->to_card()); 
                break;
            case pb_t: 
                s = subsumes(p1, c->to_pb()); 
                break;
            default: 
                break;
            }
            if (s) {
                ++m_stats.m_num_pb_subsumes;
                p1.set_learned(false);
                remove_constraint(*c);
            }
        }
    }

    literal ba_solver::get_min_occurrence_literal(card const& c) {
        unsigned occ_count = UINT_MAX;
        literal lit = null_literal;
        for (literal l : c) {
            unsigned occ_count1 = m_cnstr_use_list[l.index()].size();
            if (occ_count1 < occ_count) {
                lit = l;
                occ_count = occ_count1;
            }
        }
        return lit;
    }

    void ba_solver::card_subsumption(card& c1, literal lit) {
        literal_vector slit;
        for (constraint* c : m_cnstr_use_list[lit.index()]) {
            if (!c->is_card() || c == &c1 || c->was_removed()) {
                continue;
            }
            card& c2 = c->to_card();

            SASSERT(c1.index() != c2.index());
            if (subsumes(c1, c2, slit)) {
                if (slit.empty()) {
                    TRACE("sat", tout << "subsume cardinality\n" << c1.index() << ":" << c1 << "\n" << c2.index() << ":" << c2 << "\n";);
                    remove_constraint(c2);
                    ++m_stats.m_num_pb_subsumes;
                    c1.set_learned(false);
                }
                else {
                    TRACE("sat", tout << "self subsume cardinality\n";);
                    IF_VERBOSE(0, 
                               verbose_stream() << "self-subsume cardinality is TBD\n"; 
                               verbose_stream() << c1 << "\n";
                               verbose_stream() << c2 << "\n";);
#if 0
                    clear_watch(c2);                    
                    for (unsigned i = 0; i < c2.size(); ++i) {
                        if (slit == c2[i]) {
                            c2.swap(i, c2.size() -1);
                            break;
                        }
                    }
                    c2.set_size(c2.size() - 1);
                    init_watch(c2, true);
                    m_simplify_change = true;
#endif
                }
            }
        }
    }

    void ba_solver::clause_subsumption(card& c1, literal lit, clause_vector& removed_clauses) {
        SASSERT(!c1.was_removed());
        literal_vector slit;
        clause_use_list::iterator it = m_clause_use_list.get(lit).mk_iterator();
        while (!it.at_end()) {
            clause& c2 = it.curr();
            if (!c2.was_removed() && subsumes(c1, c2, slit)) {
                if (slit.empty()) {
                    TRACE("sat", tout << "remove\n" << c1 << "\n" << c2 << "\n";);
                    removed_clauses.push_back(&c2);
                    ++m_stats.m_num_clause_subsumes;
                    c1.set_learned(false);
                }
                else {
                    IF_VERBOSE(0, verbose_stream() << "self-subsume clause is TBD\n";);
                    // remove literal slit from c2.
                    TRACE("sat", tout << "TBD remove literals " << slit << " from " << c2 << "\n";);
                }
            }            
            it.next();
        }
    }

    void ba_solver::binary_subsumption(card& c1, literal lit) {
        if (c1.k() + 1 != c1.size()) return;
        SASSERT(is_marked(lit));
        SASSERT(!c1.was_removed());
        watch_list & wlist = get_wlist(~lit);
        watch_list::iterator it = wlist.begin();
        watch_list::iterator it2 = it;
        watch_list::iterator end = wlist.end();
        for (; it != end; ++it) {
            watched w = *it;
            if (w.is_binary_clause() && is_marked(w.get_literal())) {
                ++m_stats.m_num_bin_subsumes;
                // IF_VERBOSE(10, verbose_stream() << c1 << " subsumes (" << lit << " " << w.get_literal() << ")\n";);
                if (!w.is_binary_non_learned_clause()) {
                    c1.set_learned(false);
                }
            }
            else {
                if (it != it2) {
                    *it2 = *it;
                }
                ++it2;
            }
        }
        if (it != it2) {
            wlist.set_end(it2);
        }
    }

    void ba_solver::subsumption(card& c1) {
        if (c1.was_removed() || c1.lit() != null_literal) {
            return;
        }
        clause_vector removed_clauses;
        for (literal l : c1) mark_visited(l);  
        for (unsigned i = 0; i < std::min(c1.size(), c1.k() + 1); ++i) {
            literal lit = c1[i];            
            card_subsumption(c1, lit);
            clause_subsumption(c1, lit, removed_clauses);
            binary_subsumption(c1, lit);                   
        }
        for (literal l : c1) unmark_visited(l);
        m_clause_removed |= !removed_clauses.empty();
        for (clause *c : removed_clauses) {
            c->set_removed(true);
            m_clause_use_list.erase(*c);
        }
    }

    void ba_solver::subsumption(pb& p1) {
        if (p1.was_removed() || p1.lit() != null_literal) {
            return;
        }
        for (wliteral l : p1) {
            SASSERT(m_weights[l.second.index()] == 0);
            m_weights.setx(l.second.index(), l.first, 0);
            mark_visited(l.second);  
        }
        for (unsigned i = 0; i < p1.num_watch(); ++i) {
            subsumes(p1, p1[i].second);
        }
        for (wliteral l : p1) {
            m_weights[l.second.index()] = 0;
            unmark_visited(l.second);
        }
    }

    void ba_solver::clauses_modifed() {}

    lbool ba_solver::get_phase(bool_var v) { return l_undef; }

    /*
      \brief lit <=> conjunction of unconstrained lits
     */
    void ba_solver::assert_unconstrained(literal lit, literal_vector const& lits) {
        if (lit == null_literal) {
            for (literal l : lits) {
                if (value(l) == l_undef) {
                    s().assign(l, justification());
                }
            }
        }
        else {
            // add clauses for: lit <=> conjunction of undef literals
            SASSERT(value(lit) == l_undef);
            literal_vector cl;
            cl.push_back(lit);
            for (literal l : lits) {
                if (value(l) == l_undef) {
                    s().mk_clause(~lit, l);
                    cl.push_back(~l);
                }
            }    
            s().mk_clause(cl);
        }
    }

    extension* ba_solver::copy(solver* s) {
        ba_solver* result = alloc(ba_solver);
        result->set_solver(s);
        literal_vector lits;
        svector<wliteral> wlits;
        for (constraint* cp : m_constraints) {
            switch (cp->tag()) {
            case card_t: {
                card const& c = cp->to_card();
                lits.reset();
                for (literal l : c) lits.push_back(l);
                result->add_at_least(c.lit(), lits, c.k(), c.learned());        
                break;
            }
            case pb_t: {
                pb const& p = cp->to_pb();
                wlits.reset();
                for (wliteral w : p) {
                    wlits.push_back(w);
                }
                result->add_pb_ge(p.lit(), wlits, p.k(), p.learned());
                break;
            }
            case xor_t: {
                xor const& x = cp->to_xor();
                lits.reset();
                for (literal l : x) lits.push_back(l);
                result->add_xor(x.lit(), lits, x.learned());        
                break;
            }
            default:
                UNREACHABLE();
            }                
        }

        return result;
    }

    void ba_solver::init_use_list(ext_use_list& ul) {
        ul.init(s().num_vars());
        for (constraint const* cp : m_constraints) {
            ext_constraint_idx idx = cp->index();
            if (cp->lit() != null_literal) {
                ul.insert(cp->lit(), idx);
                ul.insert(~cp->lit(), idx);                
            }
            switch (cp->tag()) {
            case card_t: {
                card const& c = cp->to_card();
                for (literal l : c) {
                    ul.insert(l, idx);
                }
                break;
            }
            case pb_t: {
                pb const& p = cp->to_pb();
                for (wliteral w : p) {
                    ul.insert(w.second, idx);
                }
                break;
            }
            case xor_t: {
                xor const& x = cp->to_xor();
                for (literal l : x) {
                    ul.insert(l, idx);
                    ul.insert(~l, idx);
                }
                break;
            }
            default:
                UNREACHABLE();
            }                            
        }
    }

    //
    // literal is used in a clause (C or l), it
    // it occurs negatively in constraint c.
    // all literals in C are marked
    // 
    bool ba_solver::is_blocked(literal l, ext_constraint_idx idx) {
        constraint const& c = index2constraint(idx);
        simplifier& sim = s().m_simplifier;
        if (c.lit() != null_literal) return false;
        switch (c.tag()) {
        case card_t: {
            card const& ca = c.to_card();
            unsigned weight = 0;
            for (literal l2 : ca) {
                if (sim.is_marked(~l2)) ++weight;
            }
            return weight >= ca.k();
        }
        case pb_t: {
            pb const& p = c.to_pb();
            unsigned weight = 0, offset = 0;
            for (wliteral l2 : p) {
                if (~l2.second == l) {
                    offset = l2.first;
                    break;
                }
            }
            SASSERT(offset != 0);
            for (wliteral l2 : p) {
                if (sim.is_marked(~l2.second)) {
                    weight += std::min(offset, l2.first);
                }
            }
            return weight >= p.k();
        }
        default:
            break;
        }
        return false;
    }


    void ba_solver::find_mutexes(literal_vector& lits, vector<literal_vector> & mutexes) {
        literal_set slits(lits);
        bool change = false;
        for (constraint* cp : m_constraints) {
            if (!cp->is_card()) continue;
            card const& c = cp->to_card();
            if (c.size() == c.k() + 1) {
                literal_vector mux;
                for (literal lit : c) {
                    if (slits.contains(~lit)) {
                        mux.push_back(~lit);
                    }
                }
                if (mux.size() <= 1) {
                    continue;
                }

                for (literal m : mux) {
                    slits.remove(m);
                }
                change = true;
                mutexes.push_back(mux);
            }
        }        
        if (!change) return;
        lits.reset();
        for (literal l : slits) {
            lits.push_back(l);
        }
    }

    void ba_solver::display(std::ostream& out, ineq& ineq, bool values) const {
        for (unsigned i = 0; i < ineq.m_lits.size(); ++i) {
            out << ineq.m_coeffs[i] << "*" << ineq.m_lits[i] << " ";
            if (values) out << value(ineq.m_lits[i]) << " ";
        }
        out << ">= " << ineq.m_k << "\n";
    }

    void ba_solver::display(std::ostream& out, xor const& x, bool values) const {
        out << "xor " << x.lit();
        if (x.lit() != null_literal && values) {
            out << "@(" << value(x.lit());
            if (value(x.lit()) != l_undef) {
                out << ":" << lvl(x.lit());
            }
            out << "): ";
        }
        else {
            out << ": ";
        }
        for (unsigned i = 0; i < x.size(); ++i) {
            literal l = x[i];
            out << l;
            if (values) {
                out << "@(" << value(l);
                if (value(l) != l_undef) {
                    out << ":" << lvl(l);
                }
                out << ") ";
            }
            else {
                out << " ";
            }
        }        
        out << "\n";
    }

    void ba_solver::display(std::ostream& out, card const& c, bool values) const {
        if (c.lit() != null_literal) {
            if (values) {
                out << c.lit() << "[" << c.size() << "]";
                out << "@(" << value(c.lit());
                if (value(c.lit()) != l_undef) {
                    out << ":" << lvl(c.lit());
                }
                out << "): ";
            }
            else {
                out << c.lit() << " == ";
            }
        }
        for (unsigned i = 0; i < c.size(); ++i) {
            literal l = c[i];
            out << l;
            if (values) {
                out << "@(" << value(l);
                if (value(l) != l_undef) {
                    out << ":" << lvl(l);
                }
                out << ") ";
            }
            else {
                out << " ";
            }
        }
        out << ">= " << c.k()  << "\n";
    }

    std::ostream& ba_solver::display(std::ostream& out) const {
        for (constraint const* c : m_constraints) {
            out << (*c) << "\n";
        }
        if (!m_learned.empty()) {
            out << "learned:\n";
        }
        for (constraint const* c : m_learned) {
            out << (*c) << "\n";
        }
        return out;
    }

    std::ostream& ba_solver::display_justification(std::ostream& out, ext_justification_idx idx) const {
        return out << index2constraint(idx);
    }

    void ba_solver::display(std::ostream& out, constraint const& c, bool values) const {
        switch (c.tag()) {
        case card_t: display(out, c.to_card(), values); break;
        case pb_t: display(out, c.to_pb(), values); break;
        case xor_t: display(out, c.to_xor(), values); break;
        default: UNREACHABLE(); break;
        }
    }

    void ba_solver::collect_statistics(statistics& st) const {
        st.update("ba propagations", m_stats.m_num_propagations);
        st.update("ba conflicts", m_stats.m_num_conflicts);
        st.update("ba resolves", m_stats.m_num_resolves);
        st.update("ba cuts", m_stats.m_num_cut);
        st.update("ba gc", m_stats.m_num_gc);
    }

    bool ba_solver::validate_unit_propagation(card const& c, literal alit) const { 
        (void) alit;
        if (c.lit() != null_literal && value(c.lit()) != l_true) return false;
        for (unsigned i = c.k(); i < c.size(); ++i) {
            if (value(c[i]) != l_false) return false;
        }
        return true;
    }

    bool ba_solver::validate_unit_propagation(pb const& p, literal alit) const { 
        if (p.lit() != null_literal && value(p.lit()) != l_true) return false;

        unsigned sum = 0;
        TRACE("sat", display(tout << "validate: " << alit << "\n", p, true););
        for (wliteral wl : p) {
            literal lit = wl.second;
            lbool val = value(lit);
            if (val != l_false && lit != alit) {
                sum += wl.first;
            }
        }
        return sum < p.k();
    }

    bool ba_solver::validate_unit_propagation(pb const& p, literal_vector const& r, literal alit) const {
        unsigned sum = 0;
        // all elements of r are true, 
        for (literal l : r) {
            if (value(l) != l_true) return false;
        }
        // the sum of elements not in r or alit add up to less than k.
        for (wliteral wl : p) {
            if (wl.second != alit && !r.contains(~wl.second)) {
                sum += wl.first;
            }
        }
        return sum < p.k();
    }

    bool ba_solver::validate_unit_propagation(xor const& x, literal alit) const {
        if (value(x.lit()) != l_true) return false;
        for (unsigned i = 1; i < x.size(); ++i) {
            if (value(x[i]) == l_undef) return false;
        }
        return true;
    }

    bool ba_solver::validate_lemma() { 
        int64 bound64 = m_bound;
        int64 val = -bound64;
        reset_active_var_set();
        for (bool_var v : m_active_vars) {
            if (m_active_var_set.contains(v)) continue;            
            int64 coeff = get_coeff(v);
            if (coeff == 0) continue;
            m_active_var_set.insert(v);
            literal lit(v, false);
            if (coeff < 0 && value(lit) != l_true) {
                val -= coeff;
            }
            else if (coeff > 0 && value(lit) != l_false) {
                val += coeff;
            }
        }
        CTRACE("sat", val >= 0, active2pb(m_A); display(tout, m_A););
        return val < 0;
    }

    void ba_solver::reset_active_var_set() {
        while (!m_active_var_set.empty()) m_active_var_set.erase();
    }

    void ba_solver::active2pb(ineq& p) {
        reset_active_var_set();
        p.reset(m_bound);
        for (bool_var v : m_active_vars) {
            if (m_active_var_set.contains(v)) continue;            
            int64 coeff = get_coeff(v);
            if (coeff == 0) continue;
            m_active_var_set.insert(v);
            literal lit(v, coeff < 0);
            p.m_lits.push_back(lit);
            p.m_coeffs.push_back(abs(coeff));
        }
    }

    ba_solver::constraint* ba_solver::active2constraint() {
        reset_active_var_set();
        m_wlits.reset();
        uint64 sum = 0;
        if (m_bound == 1) return 0;
        if (m_overflow) return 0;
        
        for (bool_var v : m_active_vars) {
            int coeff = get_int_coeff(v);
            if (m_active_var_set.contains(v) || coeff == 0) continue;            
            m_active_var_set.insert(v);
            literal lit(v, coeff < 0);
            m_wlits.push_back(wliteral(get_abs_coeff(v), lit));
            sum += get_abs_coeff(v);
        }

        if (m_overflow || sum >= UINT_MAX/2) {
            return 0;
        }
        else {
            return add_pb_ge(null_literal, m_wlits, m_bound, true);
        }        
    }
    
    /*
      Chai Kuhlmann:
      
      a1*l1 + ... + a_n*l_n >= k
      s.t.
      a1 >= a2 >= .. >= a_n

      let m be such that

         sum_{i = 1}^{m-1} a_i < k <= sum_{i = 1}^{m}

      then 

      l1 + ... + l_n >= m

      furthermore, for the largest n' <= n, such that

         sum_{i = n'+1}^n a_i + sum_{i = 1}^{m-1} a_i < k

      then 

         l1 + ... + l_n' >= m
      
     */
    struct compare_wlit {
        bool operator()(ba_solver::wliteral l1, ba_solver::wliteral l2) const {
            return l1.first > l2.first;
        }
    };


    ba_solver::constraint* ba_solver::active2card() {
        normalize_active_coeffs();
        m_wlits.reset();
        for (bool_var v : m_active_vars) {
            int coeff = get_int_coeff(v);
            m_wlits.push_back(std::make_pair(get_abs_coeff(v), literal(v, coeff < 0)));
        }
        std::sort(m_wlits.begin(), m_wlits.end(), compare_wlit());
        unsigned k = 0;
        uint64 sum = 0, sum0 = 0;
        for (wliteral wl : m_wlits) {
            if (sum >= m_bound) break;
            sum0 = sum;
            sum += wl.first;
            ++k;
        }
        if (k == 1) {
            return 0;
        }
        while (!m_wlits.empty()) {
            wliteral wl = m_wlits.back();
            if (wl.first + sum0 >= m_bound) break;
            m_wlits.pop_back();
            sum0 += wl.first;
        }

        unsigned slack = 0;
        unsigned max_level = 0;
        unsigned num_max_level = 0;
        for (wliteral wl : m_wlits) {
            if (value(wl.second) != l_false) ++slack;
            unsigned level = lvl(wl.second);
            if (level > max_level) {
                max_level = level;
                num_max_level = 1;
            }
            else if (max_level == level) {
                ++num_max_level;
            }
        }
        if (m_overflow) return 0;

        if (slack >= k) {
#if 0
            return active2constraint();
            active2pb(m_A);
            std::cout << "not asserting\n";
            display(std::cout, m_A, true);
#endif
            return 0;
        }

        // produce asserting cardinality constraint
        literal_vector lits;
        for (wliteral wl : m_wlits) lits.push_back(wl.second);        
        constraint* c = add_at_least(null_literal, lits, k, true);      

        if (c) {
            lits.reset();
            for (wliteral wl : m_wlits) {
                if (value(wl.second) == l_false) lits.push_back(wl.second);        
            }
            unsigned glue = s().num_diff_levels(lits.size(), lits.c_ptr());

            c->set_glue(glue);
        }
        return c;
    }


    void ba_solver::justification2pb(justification const& js, literal lit, unsigned offset, ineq& ineq) {
        switch (js.get_kind()) {
        case justification::NONE:
            ineq.reset(offset);
            ineq.push(lit, offset);
            break;
        case justification::BINARY:
            ineq.reset(offset);
            ineq.push(lit, offset);
            ineq.push(js.get_literal(), offset);
            break;
        case justification::TERNARY:
            ineq.reset(offset);
            ineq.push(lit, offset);
            ineq.push(js.get_literal1(), offset);
            ineq.push(js.get_literal2(), offset);
            break;
        case justification::CLAUSE: {
            ineq.reset(offset);
            clause & c = *(s().m_cls_allocator.get_clause(js.get_clause_offset()));
            for (literal l : c) ineq.push(l, offset);
            break;
        }
        case justification::EXT_JUSTIFICATION: {
            ext_justification_idx index = js.get_ext_justification_idx();
            constraint& cnstr = index2constraint(index);
            switch (cnstr.tag()) {
            case card_t: {
                card& c = cnstr.to_card();
                ineq.reset(offset*c.k());
                for (literal l : c) ineq.push(l, offset);
                if (c.lit() != null_literal) ineq.push(~c.lit(), offset*c.k());                
                break;
            }
            case pb_t: {
                pb& p = cnstr.to_pb();
                ineq.reset(p.k());
                for (wliteral wl : p) ineq.push(wl.second, wl.first);
                if (p.lit() != null_literal) ineq.push(~p.lit(), p.k());
                break;
            }
            case xor_t: {
                xor& x = cnstr.to_xor();
                literal_vector ls;
                get_antecedents(lit, x, ls);                
                ineq.reset(offset);
                for (literal l : ls) ineq.push(~l, offset);
                literal lxor = x.lit();                
                if (lxor != null_literal) ineq.push(~lxor, offset);
                break;
            }
            default:
                UNREACHABLE();
                break;
            }
            break;
        }
        default:
            UNREACHABLE();
            break;
        }
    }


    // validate that m_A & m_B implies m_C

    bool ba_solver::validate_resolvent() {
        u_map<uint64> coeffs;
        uint64 k = m_A.m_k + m_B.m_k;
        for (unsigned i = 0; i < m_A.m_lits.size(); ++i) {
            uint64 coeff = m_A.m_coeffs[i];
            SASSERT(!coeffs.contains(m_A.m_lits[i].index()));
            coeffs.insert(m_A.m_lits[i].index(), coeff);
        }
        for (unsigned i = 0; i < m_B.m_lits.size(); ++i) {
            uint64 coeff1 = m_B.m_coeffs[i], coeff2;
            literal lit = m_B.m_lits[i];
            if (coeffs.find((~lit).index(), coeff2)) {
                if (coeff1 == coeff2) {
                    coeffs.remove((~lit).index());
                    k += coeff1;
                }
                else if (coeff1 < coeff2) {
                    coeffs.insert((~lit).index(), coeff2 - coeff1);
                    k += coeff1;
                }
                else {
                    SASSERT(coeff2 < coeff1);
                    coeffs.remove((~lit).index());
                    coeffs.insert(lit.index(), coeff1 - coeff2);
                    k += coeff2;
                }
            }
            else if (coeffs.find(lit.index(), coeff2)) {
                coeffs.insert(lit.index(), coeff1 + coeff2);
            }
            else {
                coeffs.insert(lit.index(), coeff1);
            }
        }
        // C is above the sum of A and B
        for (unsigned i = 0; i < m_C.m_lits.size(); ++i) {
            literal lit = m_C.m_lits[i];
            uint64 coeff;
            if (coeffs.find(lit.index(), coeff)) {
                if (coeff > m_C.m_coeffs[i] && m_C.m_coeffs[i] < m_C.m_k) {
                    IF_VERBOSE(0, verbose_stream() << i << ": " << m_C.m_coeffs[i] << " " << m_C.m_k << "\n";);
                    goto violated;
                }
                coeffs.remove(lit.index());
            }
        }
        if (!coeffs.empty()) goto violated;
        if (m_C.m_k > k) goto violated;
        SASSERT(coeffs.empty());
        SASSERT(m_C.m_k <= k);
        return true;

    violated:
        IF_VERBOSE(0, 
                   display(verbose_stream(), m_A);
                   display(verbose_stream(), m_B);
                   display(verbose_stream(), m_C);
                   for (auto& e : coeffs) {
                       verbose_stream() << to_literal(e.m_key) << ": " << e.m_value << "\n";
                   });
        
        return false;
    }

    bool ba_solver::validate_conflict(literal_vector const& lits, ineq& p) { 
        for (literal l : lits) {
            if (value(l) != l_false) {
                TRACE("sat", tout << "literal " << l << " is not false\n";);
                return false;
            }
        }
        uint64 value = 0;        
        for (unsigned i = 0; i < p.m_lits.size(); ++i) {
            uint64 coeff = p.m_coeffs[i];
            if (!lits.contains(p.m_lits[i])) {
                value += coeff;
            }
        }
        CTRACE("sat", value >= p.m_k, tout << "slack: " << value << " bound " << p.m_k << "\n";
               display(tout, p);
               tout << lits << "\n";);
        return value < p.m_k;
    }


    
};

