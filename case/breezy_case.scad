// Breezy v4 Case
// Board: 85.05mm x 55.05mm, 2mm corner radius (measured from KiCad source)
// Mounting holes: 2.1mm (M2 hardware!), ~1.6-1.8mm inset, slightly asymmetric
// DevKit mounts vertically on the right; USB now exits the BOTTOM edge

/* [Board Dimensions] */
board_width = 85.05;
board_height = 55.05;
board_corner_radius = 2;
board_thickness = 1.6;

/* [Mounting Holes] */
hole_diameter = 2.1;  // v4 boards; v3 was 3.2/M3
standoff_height = 5;
standoff_diameter = 6;

/* [Case Parameters] */
wall_thickness = 2;
base_thickness = 2;
lid_thickness = 2;
clearance = 0.5;  // tolerance around board (FDM pockets print smaller than modeled)
// Cavity height ABOVE the base plate - the standoffs and the board live inside
// it, so the stack-up is: 5 standoff + 1.6 board + 21.5 tallest part = 28.1mm.
// Tallest part is PS1, the Mean Well IRM-10-5 (45.7 x 25.4 x 21.5 body per its
// datasheet); next are J5 at 13.8mm (Phoenix GMKDS 3/2-7,62 "constructional
// height") and the socketed DevKit at ~13mm. 30 leaves ~2mm over the PSU.
// Was 35 - an unmeasured guess. To go lower than this, the only part in the
// way is PS1: a closed blister in the lid over its 45.7 x 25.4 footprint would
// allow ~22mm here. Do NOT cut an opening there instead - that is the mains
// section (J5, RV1, AC traces).
internal_height = 30;

/* [Cutouts - positions measured from v4 KiCad board] */
// All X centres below are pad-span centres read out of the KiCad board, NOT
// footprint anchors - see the J5 note. x_offset = centre - width/2, measured
// from the board's left edge.
//
// J1/J2/USB are TOP-OPEN SLOTS running from the board surface to the top of
// the wall, so their heights are not parameters at all. The first print had
// them as fixed-height windows and all three came out too low: the JST headers
// are B6B-XH-A (vertical/top-entry, so the cable leaves upward) and the DevKit
// USB sits ~10mm up on its 1x19 sockets, neither of which a window starting at
// board level allows for. A slot cannot be too short, the board still drops
// straight in with cables attached, and the lid closes over the top.

// J1 JST connector (top edge), pad-span centre x = 30.43.
// 17 wide, not 18, purely to leave a 1.0mm wall between this and the AC
// window next door - these two are the only pair close enough to collide.
j1_width = 17;
j1_x_offset = 21.93;

// J2 JST connector (bottom edge), pad-span centre x = 30.24
j2_width = 18;
j2_x_offset = 21.24;

// AC screw terminal (top edge, left side), Phoenix GMKDS 3/2-7,62.
// Its pads are at x 8.12 and 15.74, so the body centre is 11.93 - but the
// KiCad footprint ANCHOR is at 19.55, the body's right edge. The first print
// used the anchor as the centre, putting the window 7.62mm (one pole pitch)
// off and leaving only one terminal reachable. Body is 15.24 x 13.8 tall.
// Kept as a closed window rather than a slot: this is the mains entry.
// Width 18 (not 16): the body is 15.24 wide, and FDM pockets print smaller
// than modelled, so 16 left only 0.38mm a side - too tight, same mistake as
// the 0.3mm board clearance on the first tray.
ac_width = 18;
ac_height = 15;      // body is 13.8 above the board; a little over
ac_x_offset = 2.93;  // centre 11.93

// USB port (bottom edge - the DevKit's USB faces down)
// DevKit bay spans x 55.7-84.7; sockets J3/J4 at 57.44 and 82.89 -> centre 70.2
usb_width = 14;
usb_x_offset = 63.2;

/* [DevKit Antenna Relief] */
// The DevKit is 55mm long and the board is 55.05mm tall, so the DevKit sits
// flush with the board's top and bottom edges - there is no margin anywhere
// for the WROOM module to overhang into. And it must overhang: Espressif
// requires no PCB under the antenna (FR4 detunes it), so the module hangs off
// the DevKit's top edge, past the board outline and into the wall. Without
// this relief the lid does not close (found the hard way, first print).
// The bulge runs the full height of the wall, so the board still drops
// straight in and lifts straight out - it is NOT an enclosed window.
// 2mm of plastic is transparent at 2.4GHz, so no opening is needed; air
// around the antenna is better RF than plastic against it either way.
antenna_relief = true;
relief_depth = 8;    // how far the cavity extends past the board edge (+Y).
                     // Generous: the true overhang differs between the
                     // official DevKitC and clones, and was not measured.
relief_width = 29;   // along X = exactly the DevKit bay (x 55.7-84.7). Wider
                     // than this and the outer bulge oversteps the case side,
                     // because the bay ends 0.35mm from the board edge.
relief_x = 70.2;     // bay centre from the board's left edge (= USB centre)

/* [Pegs] */
// Locating pegs instead of M2 screws - the case is mounted flat, so the board
// only needs locating, not clamping. Pegs sit below the DevKit (peg top 11mm,
// DevKit underside ~17mm on its sockets), so they do not foul it.
use_pegs = true;
peg_diameter = 1.7;  // under the board's 2.1mm hole; tapered tip to insert
peg_height = 4;      // above the standoff

/* [Rendering] */
$fn = 32;
show_base = true;
show_lid = true;
explode = 15;  // set to 0 for assembled view

// Calculated dimensions
case_width = board_width + 2*wall_thickness + 2*clearance;
case_height = board_height + 2*wall_thickness + 2*clearance;
case_internal_width = board_width + 2*clearance;
case_internal_height = board_height + 2*clearance;

// Z landmarks. board_top is the PCB's upper face - connectors sit ON it, so
// every cutout starts there, not at the cavity floor. Forgetting the standoffs
// and the board thickness is what put the first print's windows too low.
board_top = base_thickness + standoff_height + board_thickness;  // 8.6
wall_top  = base_thickness + internal_height;                    // 32

// Slots are open at the top, so raising their floor is what shortens them.
// slot_rise trims 10mm off the opening (2026-08-04, after the second print):
// nothing needs to pass through the wall down at board level - the JST cables
// leave their vertical headers upward, and the DevKit's USB rides ~10mm up on
// its sockets - so the lower 10mm was open wall for no reason.
slot_rise = 10;
slot_z    = board_top - 0.5 + slot_rise;   // 18.1
slot_h    = wall_top - slot_z + 1;         // ...up through the top of the wall

// The AC window does NOT share slot_z. It is a fixed-height window sitting on
// the terminal body (8.6..22.4 above the floor), and its height was confirmed
// correct on the first print - raising it with the slots would have put it
// almost entirely above the connector.
ac_z = board_top - 0.5;                    // 8.1

// Mounting hole positions (from board origin, x from left / y from bottom)
// Explicit coordinates measured from the v4 KiCad board - insets vary
// 1.56-1.82mm between corners, so no computed inset.
holes = [
    [1.81, 1.82],   // bottom-left
    [83.29, 1.72],  // bottom-right
    [1.75, 53.30],  // top-left
    [83.41, 53.49]  // top-right
];

module rounded_rect(w, h, r) {
    offset(r) offset(-r) square([w, h], center=true);
}

// Antenna relief footprints. The inner one extends the cavity past the board
// edge; the outer one is bigger by a wall thickness on the three exposed sides
// so the shell still wraps it. Both overlap the main body by 1mm so the union
// and difference are clean.
module relief_inner_2d() {
    y0 = case_internal_height/2 - 1;
    y1 = case_internal_height/2 + relief_depth;
    translate([relief_x - board_width/2, (y0 + y1)/2])
        square([relief_width, y1 - y0], center=true);
}

module relief_outer_2d() {
    y0 = case_internal_height/2 - 1;
    y1 = case_internal_height/2 + relief_depth + wall_thickness;
    translate([relief_x - board_width/2, (y0 + y1)/2])
        square([relief_width + 2*wall_thickness, y1 - y0], center=true);
}

// Outer footprint of the case at any Z: the rounded body plus the bulge.
module case_outline_2d() {
    union() {
        rounded_rect(case_width, case_height, board_corner_radius + wall_thickness);
        if (antenna_relief) relief_outer_2d();
    }
}

module peg() {
    cylinder(h=peg_height - 1, d=peg_diameter);
    translate([0, 0, peg_height - 1])
        cylinder(h=1, d1=peg_diameter, d2=peg_diameter - 0.6);
}

module base() {
    difference() {
        union() {
            // Main shell
            linear_extrude(base_thickness + internal_height) {
                difference() {
                    case_outline_2d();
                    union() {
                        rounded_rect(case_internal_width, case_internal_height, board_corner_radius);
                        if (antenna_relief) relief_inner_2d();
                    }
                }
            }
            // Base plate
            linear_extrude(base_thickness) {
                case_outline_2d();
            }
            // Standoffs
            for (hole = holes) {
                translate([hole[0] - board_width/2, hole[1] - board_height/2, base_thickness]) {
                    cylinder(h=standoff_height, d=standoff_diameter);
                }
            }
            // Locating pegs on top of the standoffs
            if (use_pegs) {
                for (hole = holes) {
                    translate([hole[0] - board_width/2, hole[1] - board_height/2,
                               base_thickness + standoff_height]) {
                        peg();
                    }
                }
            }
        }

        // Screw holes in standoffs - only when not using pegs
        if (!use_pegs) {
            for (hole = holes) {
                translate([hole[0] - board_width/2, hole[1] - board_height/2, -1]) {
                    cylinder(h=base_thickness + standoff_height + 2, d=1.7);  // M2 self-tap
                    // (board holes are 2.1mm - M3 screws no longer fit through them)
                }
            }
        }

        // J1 JST slot (top edge) - open to the top of the wall
        translate([j1_x_offset - board_width/2, case_height/2 - wall_thickness - 1, slot_z]) {
            cube([j1_width, wall_thickness + 2, slot_h]);
        }

        // J2 JST slot (bottom edge) - open to the top of the wall
        translate([j2_x_offset - board_width/2, -case_height/2 - 1, slot_z]) {
            cube([j2_width, wall_thickness + 2, slot_h]);
        }

        // AC screw terminal window (top edge, left side) - a closed window,
        // not a slot, because this is the mains entry.
        translate([ac_x_offset - board_width/2, case_height/2 - wall_thickness - 1, ac_z]) {
            cube([ac_width, wall_thickness + 2, ac_height]);
        }

        // USB slot (bottom edge - the DevKit's USB faces down)
        translate([usb_x_offset - board_width/2, -case_height/2 - 1, slot_z]) {
            cube([usb_width, wall_thickness + 2, slot_h]);
        }
    }
}

module lid() {
    lip_height = 3;
    lip_clearance = 0.2;

    difference() {
        union() {
            // Main lid plate - follows the outer footprint so it also covers
            // the antenna relief, which is open at the top of the wall.
            linear_extrude(lid_thickness) {
                case_outline_2d();
            }
            // Inner lip
            translate([0, 0, -lip_height]) {
                linear_extrude(lip_height) {
                    difference() {
                        rounded_rect(case_internal_width - lip_clearance, case_internal_height - lip_clearance, board_corner_radius);
                        rounded_rect(case_internal_width - wall_thickness*2, case_internal_height - wall_thickness*2, board_corner_radius - 1);
                    }
                }
            }
        }

        // Vent holes
        for (x = [-20, 0, 20]) {
            for (y = [-15, 0, 15]) {
                translate([x, y, -1]) {
                    cylinder(h=lid_thickness + 2, d=3);
                }
            }
        }
    }
}

// Render
if (show_base) {
    base();
}

if (show_lid) {
    translate([0, 0, base_thickness + internal_height + explode]) {
        lid();
    }
}
