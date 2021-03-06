#include "BaseIntegralChannelsDetector.hpp"

#include "BaseVeryFastIntegralChannelsDetector.hpp" // for dynamic_cast<>

#include "integral_channels/IntegralChannelsForPedestrians.hpp"

#include "ModelWindowToObjectWindowConverterFactory.hpp"

#include "helpers/get_option_value.hpp"
#include "helpers/Log.hpp"

#include <boost/math/special_functions/round.hpp>

#include <algorithm> // for std::max
#include <cstdio>

namespace
{

std::ostream & log_info()
{
    return  logging::log(logging::InfoMessage, "BaseIntegralChannelsDetector");
}

std::ostream & log_debug()
{
    return  logging::log(logging::DebugMessage, "BaseIntegralChannelsDetector");
}

std::ostream & log_error()
{
    return  logging::log(logging::ErrorMessage, "BaseIntegralChannelsDetector");
}

std::ostream & log_warning()
{
    return  logging::log(logging::WarningMessage, "BaseIntegralChannelsDetector");
}

} // end of anonymous namespace


namespace doppia {

typedef AbstractObjectsDetector::detection_window_size_t detection_window_size_t;
typedef AbstractObjectsDetector::detections_t detections_t;
typedef AbstractObjectsDetector::detection_t detection_t;


boost::program_options::options_description
BaseIntegralChannelsDetector ::get_args_options()
{
    using namespace boost::program_options;

    options_description desc("BaseIntegralChannelsDetector options");

    desc.add_options()

            ("objects_detector.stixels_vertical_margin",
             value<int>()->default_value(30),
             "vertical margin, in pixels, used to filter out detections based on "
             "the stixels estimate from the previous frame (also used when only estimating ground plane)")

            ("objects_detector.stixels_scales_margin",
             value<int>()->default_value(5),
             "how many scales search around the stixel scale ? The number of scales evaluated is 2*margin. "
             "For values <= 0, all scales will be evaluated. ")

            ;

    return desc;
}

BaseIntegralChannelsDetector::BaseIntegralChannelsDetector(
        const boost::program_options::variables_map &options,
        const boost::shared_ptr<SoftCascadeOverIntegralChannelsModel> cascade_model_p_,
        const boost::shared_ptr<AbstractNonMaximalSuppression> non_maximal_suppression_p,
        const float score_threshold_,
        const int additional_border_)
    : BaseObjectsDetectorWithNonMaximalSuppression(options, non_maximal_suppression_p),
      score_threshold(score_threshold_),
      use_the_detector_model_cascade(false),
      cascade_model_p(cascade_model_p_),
      additional_border(std::max(additional_border_, 0)),
      stixels_vertical_margin(get_option_value<int>(options, "objects_detector.stixels_vertical_margin")),
      stixels_scales_margin(get_option_value<int>(options, "objects_detector.stixels_scales_margin"))
{

    if(cascade_model_p)
    {
        // IntegralChannelsForPedestrians::get_shrinking_factor() == GpuIntegralChannelsForPedestrians::get_shrinking_factor()
        if(cascade_model_p->get_shrinking_factor() != IntegralChannelsForPedestrians::get_shrinking_factor())
        {
            printf("cascade_model_p->get_shrinking_factor() == %i\n",
                   cascade_model_p->get_shrinking_factor());

            printf("(Gpu)IntegralChannelsForPedestrians::get_shrinking_factor() == %i\n",
                   IntegralChannelsForPedestrians::get_shrinking_factor());

            throw std::invalid_argument("The input model has a different shrinking factor than "
                                        "the currently used integral channels computer");
        }



        // get the detection window size
        scale_one_detection_window_size = cascade_model_p->get_model_window_size();


        // set the model to object window converter
        model_window_to_object_window_converter_p.reset(
                ModelWindowToObjectWindowConverterFactory::new_instance(cascade_model_p->get_model_window_size(),
                                                                        cascade_model_p->get_object_window()));

        {
            const bool ignore_cascade = get_option_value<bool>(options, "objects_detector.ignore_soft_cascade");
            use_the_detector_model_cascade = (ignore_cascade == false) and cascade_model_p->has_soft_cascade();

            if(use_the_detector_model_cascade)
            {
                log_info() << "Will use the model soft cascade at run time" << std::endl;
            }
            else
            {
                log_info() << "Will not use a soft cascade at run time" << std::endl;
            }

        }
    }
    else
    {
        // may receive a null cascade_model_p when invoking from MultiscalesIntegralChannelsDetector,
        // thus we do not raise an exception at this stage
    }

    return;
}


BaseIntegralChannelsDetector::~BaseIntegralChannelsDetector()
{
    // nothing to do here
    return;
}


void BaseIntegralChannelsDetector::set_stixels(const stixels_t &stixels)
{
    // we store a copy
    estimated_stixels = stixels;

    if(estimated_stixels.size() != static_cast<size_t>(get_input_width() - 2*additional_border))
    {
        throw std::invalid_argument("BaseIntegralChannelsDetector::set_stixels expects to "
                                    "receive stixels of width 1 pixel, cover the whole image width");
    }

    return;
}


void update_search_range(const AbstractObjectsDetector::ground_plane_corridor_t &estimated_ground_plane_corridor,
                         const int vertical_margin,
                         const std::vector<ScaleData> &extra_data_per_scale,
                         detector_search_ranges_t &search_ranges)
{
    static bool first_call = true;
    assert(extra_data_per_scale.size() == search_ranges.size());

    float
            pixels_count_original = 0,
            pixels_count_updated = 0;

    for(size_t scale_index=0; scale_index < search_ranges.size(); scale_index +=1)
    {
        const ScaleData &extra_data = extra_data_per_scale[scale_index];
        DetectorSearchRange &search_range = search_ranges[scale_index];

        if((search_range.max_x == 0) or (search_range.max_y == 0))
        {
            // nothing to do here
            continue;
        }

        const int detection_height = extra_data.scaled_detection_window_size.y();

        size_t min_margin_corridor_bottom_v = 0;
        float min_corridor_abs_margin = std::numeric_limits<float>::max();

        // v is the vertical pixel coordinate
        for(size_t bottom_v=0; bottom_v < estimated_ground_plane_corridor.size(); bottom_v+=1)
        {
            const int object_top_v = estimated_ground_plane_corridor[bottom_v];

            if(object_top_v < 0)
            {
                // above the horizon
                continue;
            }

            const int corridor_height = bottom_v - object_top_v;
            assert(corridor_height > 0);

            //printf("Scale index == %zi, detection_height == %i, bottom_v == %i, corridor_height == %i\n",
            //       scale_index, detection_height, bottom_v, corridor_height);

            const float corridor_abs_margin = abs(detection_height - corridor_height);
            if(corridor_abs_margin < min_corridor_abs_margin)
            {
                min_corridor_abs_margin = corridor_abs_margin;
                min_margin_corridor_bottom_v = bottom_v;
            }

        } // end of "for each vertical coordinate"


        // the search range is defined on the top-left pixel of the detection window
        // search_corridor_top - search_corridor_bottom ~= 2*vertical_margin
        const int
                search_corridor_top = estimated_ground_plane_corridor[min_margin_corridor_bottom_v] - vertical_margin,
                search_corridor_bottom = min_margin_corridor_bottom_v - detection_height + vertical_margin,
                // these variables are for debugging
                original_min_y = search_range.min_y,
                original_max_y = search_range.max_y,
                original_height = original_max_y - original_min_y,
                original_width = search_range.max_x - search_range.min_x;

        pixels_count_original += original_height*original_width;

        search_range.min_y = std::max<int>(original_min_y, search_corridor_top);
        search_range.max_y = std::min<int>(original_max_y, search_corridor_bottom);

        if(search_range.min_y > search_range.max_y)
        {
            const bool print_debug_details = false;
            if(print_debug_details)
            {
                printf("scale_index == %zi\n", scale_index);
                printf("min_corridor_abs_margin == %.3f\n", min_corridor_abs_margin);
                printf("min_margin_corridor_bottom_v == %zi\n", min_margin_corridor_bottom_v);
                printf("estimated_ground_plane_corridor[min_margin_corridor_bottom_v] == %i\n",
                       estimated_ground_plane_corridor[min_margin_corridor_bottom_v]);
                printf("Search corridor top, bottom == %i, %i\n",
                       search_corridor_top, search_corridor_bottom);
                printf("Original search_range.min_y/max_y == %i, %i\n",
                       original_min_y, original_max_y);
                printf("search_range.min_y/max_y == %i, %i\n",
                       search_range.min_y, search_range.max_y);

                printf("Something went terribly wrong inside update_search_range, "
                       "reseting the search range to original values\n");
            }

            if(first_call)
            {
                log_warning() << "At scale index " << scale_index
                              << " the detection window size is larger than the biggest ground plane corridor. "
                              << "Setting the detection search to a single line."
                              << std::endl;
            }

            if(original_min_y == 0 and original_max_y == 0)
            {
                search_range.min_y = 0;
            }
            else
            {
                search_range.min_y = std::max(0, static_cast<int>(search_range.max_y) - 1);
            }

            if(search_range.min_y > search_range.max_y)
            {
                printf("Original search_range.min_y/max_y == %i, %i\n", original_min_y, original_max_y);
                printf("search_range.min_y/max_y == %i, %i\n", search_range.min_y, search_range.max_y);
                throw std::runtime_error("Something went terribly wrong inside update_search_range");
            }
        } // end of "if search range is non-valid"


        const int
                updated_width = (search_range.max_x - search_range.min_x),
                updated_height = (search_range.max_y - search_range.min_y),
                max_reasonable_width_or_height = 5000;


        if(updated_width > max_reasonable_width_or_height) // sanity check
        {
            printf("scale index == %zi, updated_width == %i\n", scale_index, updated_width);
            throw std::runtime_error("updated_width seems unreasonably high");
        }

        if(updated_height > max_reasonable_width_or_height) // sanity check
        {
            printf("scale index == %zi, updated_height == %i\n", scale_index, updated_height);
            throw std::runtime_error("updated_height seems unreasonably high");
        }

        if(first_call)
        {
            printf("scale_index == %zi, original_height == %i, updated_height == %i\n",
                   scale_index, original_height, updated_height);
        }

        pixels_count_updated += updated_height*original_width;

    } // end of "for each search range"

    if(first_call)
    {
        printf("Expected speed gain == %.2fx (num pixels original/updated)\n",
               pixels_count_original / ( pixels_count_updated + 1));
    }

    first_call = false;
    return;
}


void BaseIntegralChannelsDetector::set_ground_plane_corridor(const ground_plane_corridor_t &corridor)
{
    // FIXME do we actually need a copy ? this only impacts the search range...
    // we store a copy
    estimated_ground_plane_corridor = corridor;

    if(estimated_ground_plane_corridor.size() != static_cast<size_t>(get_input_height() - 2*additional_border))
    {
        printf("estimated_ground_plane_corridor.size() == %zi\n", estimated_ground_plane_corridor.size());
        printf("get_input_height() == %zi\n", get_input_height());
        printf("(get_input_height() - 2*additional_border) == %zi\n", get_input_height() - 2*additional_border);

        throw std::invalid_argument("BaseIntegralChannelsDetector::set_ground_plane_corridor expects to "
                                    "receive a vector covering the whole image height");
    }

    if(additional_border != 0)
    {
        throw std::runtime_error("update_search_range for additional_border !=0 not yet implemented");
    }
    update_search_range(estimated_ground_plane_corridor,
                        stixels_vertical_margin,
                        extra_data_per_scale, search_ranges);

    // FIXME how to avoid this recomputation ?
    // we recompute the extra data (since it contains the scaled search ranges)
    compute_extra_data_per_scale(get_input_width(), get_input_height());
    return;
}



void BaseIntegralChannelsDetector::compute_scaled_detection_cascades()
{
    //printf("BaseIntegralChannelsDetector::compute_scaled_detection_cascades\n");
    assert(cascade_model_p);

    detection_cascade_per_scale.clear();
    detection_stump_cascade_per_scale.clear();
    detector_cascade_relative_scale_per_scale.clear();
    detection_window_size_per_scale.clear();
    original_detection_window_scales.clear();

    const size_t num_scales = search_ranges.size();
    detection_cascade_per_scale.reserve(num_scales);
    detection_stump_cascade_per_scale.reserve(num_scales);
    detector_cascade_relative_scale_per_scale.reserve(num_scales);
    detection_window_size_per_scale.reserve(num_scales);
    original_detection_window_scales.reserve(num_scales);


    for(size_t scale_index=0; scale_index < num_scales; scale_index+=1)
    {
        DetectorSearchRange &search_range = search_ranges[scale_index];
        original_detection_window_scales.push_back(search_range.detection_window_scale);

        const float relative_scale = 1.0f; // we rescale the images, not the the features
        const cascade_stages_t cascade_stages = cascade_model_p->get_rescaled_fast_stages(relative_scale);
        detection_cascade_per_scale.push_back(cascade_stages);

        const stump_cascade_stages_t stump_cascade_stages = cascade_model_p->get_rescaled_stump_stages(relative_scale);
        detection_stump_cascade_per_scale.push_back(stump_cascade_stages);

        detector_cascade_relative_scale_per_scale.push_back(relative_scale);
        detection_window_size_per_scale.push_back(scale_one_detection_window_size);
    } // end of "for each search range"

    return;
}


void BaseIntegralChannelsDetector::compute_extra_data_per_scale(
        const size_t input_width, const size_t input_height)
{
    static bool first_call = true;
    using boost::math::iround;

    extra_data_per_scale.clear();
    extra_data_per_scale.reserve(search_ranges.size());

    // IntegralChannelsForPedestrians::get_shrinking_factor() == GpuIntegralChannelsForPedestrians::get_shrinking_factor()
    const float channels_resizing_factor = 1.0f/IntegralChannelsForPedestrians::get_shrinking_factor();

    for(size_t scale_index=0; scale_index < search_ranges.size(); scale_index+=1)
    {
        const DetectorSearchRange &original_search_range = search_ranges[scale_index];

        ScaleData extra_data;

        const float
                input_to_input_scaled = 1.0f/original_search_range.detection_window_scale,
                input_to_input_scaled_ratio = 1.0f/original_search_range.detection_window_ratio,
                input_to_input_scaled_x = input_to_input_scaled * input_to_input_scaled_ratio;

        // update the scaled input sizes
        {
            // ratio is defined  as width/height; we apply the "inverse ratio"
            const size_t
                    scaled_x = input_width*input_to_input_scaled_x,
                    scaled_y = input_height*input_to_input_scaled;


            // FIXME move the size checks from GpuIntegralChannelsDetector::resize_input_and_compute_integral_channels into a function here
            extra_data.scaled_input_image_size = image_size_t(scaled_x, scaled_y);
        }


        // update the scaled search ranges and strides
        {
            const float
                    detection_window_scale = original_detection_window_scales[scale_index],
                    input_to_channel_scale = input_to_input_scaled*channels_resizing_factor,
                    stride_scaling = detection_window_scale*input_to_channel_scale;

            extra_data.stride = stride_t(
                                    std::max<stride_t::coordinate_t>(1, iround(x_stride*stride_scaling)),
                                    std::max<stride_t::coordinate_t>(1, iround(y_stride*stride_scaling)));
            if(first_call)
            {
                printf("Detection window scale %.3f has strides (x,y) == (%.3f, %.3f) [image pixels] =>\t(%i, %i) [channel pixels]\n",
                       detection_window_scale,
                       x_stride*stride_scaling, y_stride*stride_scaling,
                       extra_data.stride.x(),  extra_data.stride.y());
            }

            // resize the search range based on the new image size
            extra_data.scaled_search_range =
                    original_search_range.get_rescaled(input_to_channel_scale, input_to_input_scaled_ratio);
        }

        // update the scaled detection window sizes
        {
            const detection_window_size_t &original_detection_window_size = detection_window_size_per_scale[scale_index];
            const float
                    original_window_scale = original_search_range.detection_window_scale,
                    original_window_ratio = original_search_range.detection_window_ratio,
                    original_window_scale_x = original_window_scale*original_window_ratio;

            const detection_window_size_t::coordinate_t
                    detection_width = iround(original_detection_window_size.x()*original_window_scale_x),
                    detection_height = iround(original_detection_window_size.y()*original_window_scale);

            extra_data.scaled_detection_window_size = detection_window_size_t(detection_width, detection_height);
        }

        extra_data_per_scale.push_back(extra_data);
    } // end of "for each search range"


    if(dynamic_cast<BaseVeryFastIntegralChannelsDetector *>(this) != NULL)
    {
        // uses centered windows, so the current sanity check does not apply
    }
    else
    {
        // sanity check
        check_extra_data_per_scale();
    }

    first_call = false;
    return;
} // end of BaseIntegralChannelsDetector::compute_extra_data_per_scale



void BaseIntegralChannelsDetector::check_extra_data_per_scale()
{

    static size_t num_warnings = 0;
    const size_t max_num_warnings = search_ranges.size() * 2;

    if(extra_data_per_scale.size() != search_ranges.size())
    {
        throw std::runtime_error("BaseIntegralChannelsDetector::check_extra_data_per_scale "
                                 "(extra_data_per_scale.size() != search_ranges.size())");
    }

    // IntegralChannelsForPedestrians::get_shrinking_factor() == GpuIntegralChannelsForPedestrians::get_shrinking_factor()
    const float channels_resizing_factor = 1.0f/IntegralChannelsForPedestrians::get_shrinking_factor();

    for(size_t scale_index=0; scale_index < search_ranges.size(); scale_index+=1)
    {

        const ScaleData &extra_data = extra_data_per_scale[scale_index];
        const image_size_t &scaled_input_size = extra_data.scaled_input_image_size;
        const DetectorSearchRange &scaled_search_range = extra_data.scaled_search_range;
        //const detection_window_size_t &scaled_detection_window_size = extra_data.scaled_detection_window_size;

        const detection_window_size_t &detection_window_size = detection_window_size_per_scale[scale_index];
        const float detector_relative_scale = detector_cascade_relative_scale_per_scale[scale_index];

        detection_window_size_t scaled_detection_window_size;
        // detection_window_size is relative to the original image dimensions
        scaled_detection_window_size.x(detection_window_size.x()*detector_relative_scale*channels_resizing_factor);
        scaled_detection_window_size.y(detection_window_size.y()*detector_relative_scale*channels_resizing_factor);

        if((scaled_search_range.max_y > 0) and (scaled_search_range.max_x > 0))
        {   // assert <= is correct, since we want to check the data access

            const int
                    scaled_height = scaled_input_size.y()*channels_resizing_factor,
                    scaled_width = scaled_input_size.x()*channels_resizing_factor,
                    delta_y = scaled_height -(scaled_search_range.max_y + scaled_detection_window_size.y()),
                    delta_x = scaled_width -(scaled_search_range.max_x + scaled_detection_window_size.x()),
                    max_abs_delta = 10;

            if(delta_y < 0)
            {
                printf("scale index == %zi, scale == %.3f\n",
                       scale_index, search_ranges[scale_index].detection_window_scale);

                printf("(scaled_search_range.max_y + scaled_detection_window_size.y()) == %i\n",
                       (scaled_search_range.max_y + scaled_detection_window_size.y()));
                printf("resized_input_size.y() == %i\n", scaled_height);
                throw std::runtime_error("BaseIntegralChannelsDetector::check_extra_data_per_scale "
                                         "failed the y axis safety check");
            }

            if(delta_x < 0)
            {
                printf("(scaled_search_range.max_x + scaled_detection_window_size.x()) == %i\n",
                       (scaled_search_range.max_x + scaled_detection_window_size.x()));
                printf("resized_input_size.x() == %i\n", scaled_width);
                throw std::runtime_error("BaseIntegralChannelsDetector::compute_scaled_search_range_and_strides "
                                         "failed the x axis safety check");
            }

            const bool not_using_stixels = estimated_ground_plane_corridor.empty();
            if(not_using_stixels and (delta_y > max_abs_delta))
            {
                // the margin between the y border is too wide, something smells wrong

                if(false)
                {
                    throw std::runtime_error("BaseIntegralChannelsDetector::compute_scaled_search_range_and_strides "
                                             "failed the max_delta_y sanity check");
                }
                else if(false or (num_warnings < max_num_warnings))
                {
                    log_warning() << "The y-margin between search_range + detection_window_size "
                                     "and the image border is suspiciously large (" << delta_y << " pixels)" << std::endl;

                    num_warnings += 1;
                }
            }

        }

    } // end of "for each search range"

    return;
}

// ~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~


SearchRangeScaleComparator::SearchRangeScaleComparator(const detector_search_ranges_t &search_ranges_)
    : search_ranges(search_ranges_)
{
    // nothing to do here
    return;
}

SearchRangeScaleComparator::~SearchRangeScaleComparator()
{
    // nothing to do here
    return;
}

bool SearchRangeScaleComparator::operator()(const size_t a, const size_t b)
{
    return search_ranges[a].detection_window_scale < search_ranges[b].detection_window_scale;
}


// ~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~


void add_detection(
        const detection_t::coordinate_t detection_col, const detection_t::coordinate_t detection_row,
        const detection_t::coordinate_t detection_width, const detection_t::coordinate_t detection_height,
        const float detection_score,
        AbstractObjectsDetector::detections_t &detections)
{
    detection_t detection;

    // set the detection_window
    detection_t::rectangle_t &box = detection.bounding_box;
    box.min_corner().x(detection_col);
    box.min_corner().y(detection_row);
    box.max_corner().x(detection_col + std::max<detection_t::coordinate_t>(1, detection_width));
    box.max_corner().y(detection_row + std::max<detection_t::coordinate_t>(1, detection_height));

    const bool print_detection_area = false;
    if(print_detection_area)
    {
        printf("rectangle_area(detection.bounding_box) == %.3f\n",
               rectangle_area(detection.bounding_box));
    }

    // set the detection score
    detection.score = detection_score;
    detection.object_class = detection_t::Pedestrian;

    detections.push_back(detection); // will copy the detection instance
    return;
}


void add_detection(
        const boost::uint16_t detection_col, const boost::uint16_t detection_row, const float detection_score,
        const ScaleData &scale_data,
        detections_t &detections)
{
    using boost::math::iround;

    const DetectorSearchRange &scaled_search_range = scale_data.scaled_search_range;
    const detection_window_size_t &scaled_detection_window_size = scale_data.scaled_detection_window_size;

    detection_t::coordinate_t original_col, original_row;

    // map the detection point back the input image coordinates
    original_col = iround(detection_col/(scaled_search_range.range_scaling*scaled_search_range.range_ratio));
    original_row = iround(detection_row/scaled_search_range.range_scaling);

    //printf("Detection at coordinates %i,%i\n", col, row); // just for debugging

    add_detection(original_col, original_row,
                  scaled_detection_window_size.x(), scaled_detection_window_size.y(),
                  detection_score, detections);
    return;
}



void add_detection_for_bootstrapping(
        const boost::uint16_t detection_col, const boost::uint16_t detection_row, const float detection_score,
        const AbstractObjectsDetector::detection_window_size_t &original_detection_window_size,
        AbstractObjectsDetector::detections_t &detections)
{
    detection_t::coordinate_t original_col, original_row, detection_width, detection_height;

    // for the bootstrapping_lib we need the detections in the rescaled image coordinates
    // not in the input image coordinates
    const int shrinking_factor = IntegralChannelsForPedestrians::get_shrinking_factor();

    // in the boostrapping case, we simply ignore the scale and ratio information
    original_col = detection_col;
    original_row = detection_row;
    detection_width = original_detection_window_size.x()/shrinking_factor;
    detection_height = original_detection_window_size.y()/shrinking_factor;

    add_detection(original_col, original_row, detection_width, detection_height,
                  detection_score, detections);
    return;
}






} // end of namespace doppia
