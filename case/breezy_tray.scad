// Breezy v4 Tray
// Shallow tray to hold board and protect from 220V contacts
// Board: 85.05mm x 55.05mm, 2mm corner radius (v4, measured from KiCad source)
// NOTE: v4 mounting holes are 2.1mm (not 3.2mm as in v3)

/* [Board Dimensions] */
board_width = 85.05;
board_height = 55.05;
board_corner_radius = 2;
board_thickness = 1.6;

/* [Mounting Holes] */
hole_diameter = 2.1;  // v4 boards; v3 was 3.2
standoff_height = 5;
standoff_diameter = 6;

/* [Pegs] */
use_pegs = true;
peg_diameter = 1.7;  // slightly under 2.1mm hole; fragile at this size -
                     // if pegs snap, set use_pegs=false and use M2 screws
peg_height = 4;      // above standoff

/* [Tray Parameters] */
wall_thickness = 2;
base_thickness = 2;
wall_height = 8;  // low walls, just enough to hold board
clearance = 0.5;  // FDM pockets print smaller than modeled; 0.3 was too tight

/* [Rendering] */
$fn = 32;

// Calculated dimensions
tray_width = board_width + 2*wall_thickness + 2*clearance;
tray_height = board_height + 2*wall_thickness + 2*clearance;

// Mounting hole positions (from board origin, x from left / y from bottom)
// Explicit coordinates measured from the v4 KiCad board - the four holes are
// NOT perfectly symmetric (insets vary 1.56-1.82mm), so no computed inset.
holes = [
    [1.81, 1.82],
    [83.29, 1.72],
    [1.75, 53.30],
    [83.41, 53.49]
];

module rounded_rect(w, h, r) {
    offset(r) offset(-r) square([w, h], center=true);
}

module peg() {
    // straight shank + tapered tip for lead-in
    cylinder(h=peg_height - 1, d=peg_diameter);
    translate([0, 0, peg_height - 1])
        cylinder(h=1, d1=peg_diameter, d2=peg_diameter - 0.6);
}

module tray() {
    difference() {
        union() {
            // Base plate
            linear_extrude(base_thickness) {
                rounded_rect(tray_width, tray_height, board_corner_radius + wall_thickness);
            }

            // Low walls
            linear_extrude(base_thickness + wall_height) {
                difference() {
                    rounded_rect(tray_width, tray_height, board_corner_radius + wall_thickness);
                    rounded_rect(board_width + 2*clearance, board_height + 2*clearance, board_corner_radius);
                }
            }

            // Standoffs
            for (hole = holes) {
                translate([hole[0] - board_width/2, hole[1] - board_height/2, base_thickness]) {
                    cylinder(h=standoff_height, d=standoff_diameter);
                }
            }

            // Pegs on standoffs
            if (use_pegs) {
                for (hole = holes) {
                    translate([hole[0] - board_width/2, hole[1] - board_height/2, base_thickness + standoff_height]) {
                        peg();
                    }
                }
            }
        }

        // Orientation notch in the wall top, aligned with the AC screw terminal
        // (J5). Insert the board with its screw terminal at the notch - the
        // mounting holes are slightly asymmetric, so 180° insertion binds.
        translate([19.6 - board_width/2 - 2, board_height/2 + clearance - 0.01,
                   base_thickness + wall_height - 1.5]) {
            cube([4, wall_thickness + 2, 3]);
        }

        // Screw holes (only if not using pegs)
        if (!use_pegs) {
            for (hole = holes) {
                translate([hole[0] - board_width/2, hole[1] - board_height/2, -1]) {
                    cylinder(h=base_thickness + standoff_height + 2, d=2.5);
                }
            }
        }
    }
}

tray();
