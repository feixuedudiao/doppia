// Boost.Geometry (aka GGL, Generic Geometry Library)
//
// Copyright Barend Gehrels 2007-2009, Geodan, Amsterdam, the Netherlands.
// Copyright Bruno Lalande 2008, 2009
// Use, modification and distribution is subject to the Boost Software License,
// Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_GEOMETRY_MULTI_ALGORITHMS_AREA_HPP
#define BOOST_GEOMETRY_MULTI_ALGORITHMS_AREA_HPP


#include <boost/range/metafunctions.hpp>

#include <boost/geometry/algorithms/area.hpp>
#include <boost/geometry/multi/core/point_type.hpp>
#include <boost/geometry/multi/algorithms/detail/multi_sum.hpp>


namespace boost { namespace geometry
{


#ifndef DOXYGEN_NO_DISPATCH
namespace dispatch
{
template <typename MultiGeometry, typename Strategy>
struct area<multi_polygon_tag, MultiGeometry, Strategy>
    : detail::multi_sum
        <
            typename Strategy::return_type,
            MultiGeometry,
            Strategy,
            area
                <
                    polygon_tag,
                    typename boost::range_value<MultiGeometry>::type,
                    Strategy
                >
    >
{};


} // namespace dispatch
#endif


}} // namespace boost::geometry


#endif // BOOST_GEOMETRY_MULTI_ALGORITHMS_AREA_HPP
