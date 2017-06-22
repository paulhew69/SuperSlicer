#ifndef slic3r_GCode_hpp_
#define slic3r_GCode_hpp_

#include "libslic3r.h"
#include "ExPolygon.hpp"
#include "GCodeWriter.hpp"
#include "Layer.hpp"
#include "MotionPlanner.hpp"
#include "Point.hpp"
#include "PlaceholderParser.hpp"
#include "Print.hpp"
#include "PrintConfig.hpp"
#include "GCode/CoolingBuffer.hpp"
#include "GCode/PressureEqualizer.hpp"
#include "GCode/SpiralVase.hpp"
#include "GCode/ToolOrdering.hpp"
#include "GCode/WipeTower.hpp"
#include "EdgeGrid.hpp"

#include <memory>
#include <string>

namespace Slic3r {

// Forward declarations.
class GCode;

class AvoidCrossingPerimeters {
public:
    
    // this flag triggers the use of the external configuration space
    bool use_external_mp;
    bool use_external_mp_once;  // just for the next travel move
    
    // this flag disables avoid_crossing_perimeters just for the next travel move
    // we enable it by default for the first travel move in print
    bool disable_once;
    
    AvoidCrossingPerimeters() : use_external_mp(false), use_external_mp_once(false), disable_once(true) {}
    ~AvoidCrossingPerimeters() {}

    void init_external_mp(const ExPolygons &islands) { m_external_mp = Slic3r::make_unique<MotionPlanner>(islands); }
    void init_layer_mp(const ExPolygons &islands) { m_layer_mp = Slic3r::make_unique<MotionPlanner>(islands); }

    Polyline travel_to(const GCode &gcodegen, const Point &point);

private:
    std::unique_ptr<MotionPlanner> m_external_mp;
    std::unique_ptr<MotionPlanner> m_layer_mp;
};

class OozePrevention {
public:
    bool enable;
    Points standby_points;
    
    OozePrevention() : enable(false) {}
    std::string pre_toolchange(GCode &gcodegen);
    std::string post_toolchange(GCode &gcodegen);
    
private:
    int _get_temp(GCode &gcodegen);
};

class Wipe {
public:
    bool enable;
    Polyline path;
    
    Wipe() : enable(false) {}
    bool has_path() const { return !this->path.points.empty(); }
    void reset_path() { this->path = Polyline(); }
    std::string wipe(GCode &gcodegen, bool toolchange = false);
};

class WipeTowerIntegration {
public:
    WipeTowerIntegration(
        const PrintConfig                                           &print_config,
        const std::vector<std::vector<WipeTower::ToolChangeResult>> &tool_changes,
        const WipeTower::ToolChangeResult                           &final_purge) :
        m_left(float(print_config.wipe_tower_x.value)),
        m_right(float(print_config.wipe_tower_x.value + print_config.wipe_tower_width.value)),
        m_tool_changes(tool_changes),
        m_final_purge(final_purge),
        m_layer_idx(-1),
        m_tool_change_idx(0),
        m_brim_done(false) {}

    void next_layer() { ++ m_layer_idx; m_tool_change_idx = 0; }
    std::string tool_change(GCode &gcodegen, int extruder_id, bool finish_layer);
    std::string finalize(GCode &gcodegen);

private:
    WipeTowerIntegration& operator=(const WipeTowerIntegration&);
    std::string append_tcr(GCode &gcodegen, const WipeTower::ToolChangeResult &tcr, int new_extruder_id) const;

    // Left / right edges of the wipe tower, for the planning of wipe moves.
    const float                                                  m_left;
    const float                                                  m_right;
    // Reference to cached values at the Printer class.
    const std::vector<std::vector<WipeTower::ToolChangeResult>> &m_tool_changes;
    const WipeTower::ToolChangeResult                           &m_final_purge;
    // Current layer index.
    int                                                          m_layer_idx;
    int                                                          m_tool_change_idx;
    bool                                                         m_brim_done;
};

struct ElapsedTime
{
    ElapsedTime() { this->reset(); }
    void reset() { total = bridges = external_perimeters = travel = other = 0.f; }

    ElapsedTime& operator+=(const ElapsedTime &rhs) {
        this->total                 += rhs.total;
        this->bridges               += rhs.bridges;
        this->external_perimeters   += rhs.external_perimeters;
        this->travel                += rhs.travel;
        this->other                 += rhs.other;
        return *this;
    }

    float   total;
    float   bridges;
    float   external_perimeters;
    float   travel;
    float   other;
};

class GCode {
public:        
    GCode() : 
        m_enable_loop_clipping(true), 
        m_enable_cooling_markers(false), 
        m_enable_extrusion_role_markers(false), 
        m_enable_analyzer_markers(false),
        m_layer_count(0),
        m_layer_index(-1), 
        m_layer(nullptr), 
        m_volumetric_speed(0),
        m_last_pos_defined(false),
        m_last_extrusion_role(erNone),
        m_brim_done(false),
        m_second_layer_things_done(false),
        m_last_obj_copy(nullptr, Point(std::numeric_limits<coord_t>::max(), std::numeric_limits<coord_t>::max()))
        {}
    ~GCode() {}

    bool            do_export(FILE *file, Print &print);

    // Exported for the helper classes (OozePrevention, Wipe) and for the Perl binding for unit tests.
    const Pointf&   origin() const { return m_origin; }
    void            set_origin(const Pointf &pointf);
    void            set_origin(const coordf_t x, const coordf_t y) { this->set_origin(Pointf(x, y)); }
    const Point&    last_pos() const { return m_last_pos; }
    Pointf          point_to_gcode(const Point &point) const;
    Point           gcode_to_point(const Pointf &point) const;
    const FullPrintConfig &config() const { return m_config; }
    const Layer*    layer() const { return m_layer; }
    GCodeWriter&    writer() { return m_writer; }
    bool            enable_cooling_markers() const { return m_enable_cooling_markers; }
    ElapsedTime     get_reset_elapsed_time() { ElapsedTime et = this->m_elapsed_time; this->m_elapsed_time.reset(); return et; }

    // For Perl bindings, to be used exclusively by unit tests.
    unsigned int    layer_count() const { return m_layer_count; }
    void            set_layer_count(unsigned int value) { m_layer_count = value; }
    float           elapsed_time() const { return m_elapsed_time.total; }
    void            set_elapsed_time(float value) { m_elapsed_time.total = value; }
    void            apply_print_config(const PrintConfig &print_config);

protected:
    // Object and support extrusions of the same PrintObject at the same print_z.
    struct LayerToPrint
    {
        LayerToPrint() : object_layer(nullptr), support_layer(nullptr) {}
        const Layer          *object_layer;
        const SupportLayer   *support_layer;
        const Layer*          layer() const { return (object_layer != nullptr) ? object_layer : support_layer; }
        const PrintObject*    object() const { return (this->layer() != nullptr) ? this->layer()->object() : nullptr; }
        coordf_t              print_z() const { return (object_layer != nullptr && support_layer != nullptr) ? 0.5 * (object_layer->print_z + support_layer->print_z) : this->layer()->print_z; }
    };
    static std::vector<GCode::LayerToPrint>                            collect_layers_to_print(const PrintObject &object);
    static std::vector<std::pair<coordf_t, std::vector<LayerToPrint>>> collect_layers_to_print(const Print &print);
    void            process_layer(
        // Write into the output file.
        FILE                            *file,
        const Print                     &print,
        // Set of object & print layers of the same PrintObject and with the same print_z.
        const std::vector<LayerToPrint> &layers,
        const ToolOrdering::LayerTools  &layer_tools,
        // If set to size_t(-1), then print all copies of all objects.
        // Otherwise print a single copy of a single object.
        const size_t                     single_object_idx = size_t(-1));

    void            set_last_pos(const Point &pos) { m_last_pos = pos; m_last_pos_defined = true; }
    bool            last_pos_defined() const { return m_last_pos_defined; }
    void            set_extruders(const std::vector<unsigned int> &extruder_ids);
    std::string     preamble();
    std::string     change_layer(coordf_t print_z);
    std::string     extrude_entity(const ExtrusionEntity &entity, std::string description = "", double speed = -1., std::unique_ptr<EdgeGrid::Grid> *lower_layer_edge_grid = nullptr);
    std::string     extrude_loop(ExtrusionLoop loop, std::string description, double speed = -1., std::unique_ptr<EdgeGrid::Grid> *lower_layer_edge_grid = nullptr);
    std::string     extrude_multi_path(ExtrusionMultiPath multipath, std::string description = "", double speed = -1.);
    std::string     extrude_path(ExtrusionPath path, std::string description = "", double speed = -1.);

    // Extruding multiple objects with soluble / non-soluble / combined supports
    // on a multi-material printer, trying to minimize tool switches.
    // Following structures sort extrusions by the extruder ID, by an order of objects and object islands.
    struct ObjectByExtruder
    {
        ObjectByExtruder() : support(nullptr), support_extrusion_role(erNone) {}
        const ExtrusionEntityCollection  *support;
        // erSupportMaterial / erSupportMaterialInterface or erMixed.
        ExtrusionRole                     support_extrusion_role;

        struct Island
        {
            struct Region {
                ExtrusionEntityCollection perimeters;
                ExtrusionEntityCollection infills;
            };
            std::vector<Region> by_region;
        };
        std::vector<Island>         islands;
    };
    std::string     extrude_perimeters(const Print &print, const std::vector<ObjectByExtruder::Island::Region> &by_region, std::unique_ptr<EdgeGrid::Grid> &lower_layer_edge_grid);
    std::string     extrude_infill(const Print &print, const std::vector<ObjectByExtruder::Island::Region> &by_region);
    std::string     extrude_support(const ExtrusionEntityCollection &support_fills);

    std::string     travel_to(const Point &point, ExtrusionRole role, std::string comment);
    bool            needs_retraction(const Polyline &travel, ExtrusionRole role = erNone);
    std::string     retract(bool toolchange = false);
    std::string     unretract() { return m_writer.unlift() + m_writer.unretract(); }
    std::string     set_extruder(unsigned int extruder_id);

    /* Origin of print coordinates expressed in unscaled G-code coordinates.
       This affects the input arguments supplied to the extrude*() and travel_to()
       methods. */
    Pointf                              m_origin;
    FullPrintConfig                     m_config;
    GCodeWriter                         m_writer;
    PlaceholderParser                   m_placeholder_parser;
    OozePrevention                      m_ooze_prevention;
    Wipe                                m_wipe;
    AvoidCrossingPerimeters             m_avoid_crossing_perimeters;
    bool                                m_enable_loop_clipping;
    // If enabled, the G-code generator will put following comments at the ends
    // of the G-code lines: _EXTRUDE_SET_SPEED, _WIPE, _BRIDGE_FAN_START, _BRIDGE_FAN_END
    // Those comments are received and consumed (removed from the G-code) by the CoolingBuffer.pm Perl module.
    bool                                m_enable_cooling_markers;
    // Markers for the Pressure Equalizer to recognize the extrusion type.
    // The Pressure Equalizer removes the markers from the final G-code.
    bool                                m_enable_extrusion_role_markers;
    // Extended markers for the G-code Analyzer.
    // The G-code Analyzer will remove these comments from the final G-code.
    bool                                m_enable_analyzer_markers;
    // How many times will change_layer() be called?
    // change_layer() will update the progress bar.
    unsigned int                        m_layer_count;
    // Progress bar indicator. Increments from -1 up to layer_count.
    int                                 m_layer_index;
    // Current layer processed. Insequential printing mode, only a single copy will be printed.
    // In non-sequential mode, all its copies will be printed.
    const Layer*                        m_layer;
    std::map<const PrintObject*,Point>  m_seam_position;
    // Used by the CoolingBuffer G-code filter to calculate time spent per layer change.
    // This value is not quite precise. First it only accouts for extrusion moves and travel moves,
    // it does not account for wipe, retract / unretract moves.
    // second it does not account for the velocity profiles of the printer.
    ElapsedTime                         m_elapsed_time;
    double                              m_volumetric_speed;
    // Support for the extrusion role markers. Which marker is active?
    ExtrusionRole                       m_last_extrusion_role;

    Point                               m_last_pos;
    bool                                m_last_pos_defined;

    std::unique_ptr<CoolingBuffer>      m_cooling_buffer;
    std::unique_ptr<SpiralVase>         m_spiral_vase;
    std::unique_ptr<PressureEqualizer>  m_pressure_equalizer;
    std::unique_ptr<WipeTowerIntegration> m_wipe_tower;

    // Heights at which the skirt has already been extruded.
    std::vector<coordf_t>               m_skirt_done;
    // Has the brim been extruded already? Brim is being extruded only for the first object of a multi-object print.
    bool                                m_brim_done;
    // Flag indicating whether the nozzle temperature changes from 1st to 2nd layer were performed.
    bool                                m_second_layer_things_done;
    // Index of a last object copy extruded.
    std::pair<const PrintObject*, Point> m_last_obj_copy;

    std::string _extrude(const ExtrusionPath &path, std::string description = "", double speed = -1);
    void _print_first_layer_extruder_temperatures(FILE *file, Print &print, unsigned int first_printing_extruder_id, bool wait);
    // this flag triggers first layer speeds
    bool                                on_first_layer() const { return m_layer != nullptr && m_layer->id() == 0; }

    std::string filter(std::string &&gcode, bool flush);

    friend ObjectByExtruder& object_by_extruder(
        std::map<unsigned int, std::vector<ObjectByExtruder>> &by_extruder, 
        unsigned int                                           extruder_id, 
        size_t                                                 object_idx, 
        size_t                                                 num_objects);
    friend std::vector<ObjectByExtruder::Island>& object_islands_by_extruder(
        std::map<unsigned int, std::vector<ObjectByExtruder>>  &by_extruder, 
        unsigned int                                            extruder_id, 
        size_t                                                  object_idx, 
        size_t                                                  num_objects,
        size_t                                                  num_islands);

    friend class WipeTowerIntegration;
};

}

#endif
