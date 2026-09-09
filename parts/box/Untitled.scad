// =============================================================================
// BOITIER BIRDIE 100MM - V14
// =============================================================================

$fn = 100;
part = "lid"; // Remplace par "case" ou "lid"
box_d       = 100.0;
wall        = 2.5;
base_h      = 2.5;
box_h       = 38.0; 
lid_h       = 2.5;

// Inserts M3
insert_d    = 4.0;
insert_col_d= 8.5;
insert_r    = 44.5; 

if (part == "all") {
    case_body();
    translate([box_d + 15, 0, 0]) lid();
} else if (part == "case") {
    case_body();
} else if (part == "lid") {
    lid();
}

// =============================================================================
// MODULE : BOITIER
// =============================================================================
module case_body() {
    difference() {
        // Corps de la boite
        hull() {
            translate([0, 0, 2]) cylinder(d = box_d, h = box_h - 2);
            translate([0, 0, 1]) cylinder(d = box_d - 2, h = 1);
            cylinder(d = box_d - 4, h = 1);
        }

        // Cavité intérieure
        translate([0, 0, base_h])
            cylinder(d = box_d - 2 * wall, h = box_h + 5);

        // 1. TROU MOTEUR FORME "DOUBLE ROND"
        translate([0, 0, -1]) {
            cylinder(d = 12.8, h = base_h + 2); 
            translate([0, -5.8, 0]) 
                cylinder(d = 7.5, h = base_h + 2); 
        }

        // Découpe USB-C
        translate([-box_d/2 + 1, 0, base_h + 3.5])
        rotate([0, 90, 0])
        linear_extrude(wall + 4, center = true)
            offset(r = 1.2) square([3.2, 8.8], center = true);

        // Ouïes aération SCD41
        for (i = [-3 : 3]) {
            for (z = [12, 17, 22]) {
                translate([i * 5.0, box_d/2 - wall - 1, base_h + z])
                rotate([-90, 0, 0])
                    cylinder(d = 2.5, h = wall + 2);
            }
        }
    }

    // =========================================================================
    // AJOUTS INTERNES 
    // =========================================================================

    // 2. COLONNES POUR INSERTS 
    for (a = [45, 135, 225, 315]) {
        rotate([0, 0, a])
        translate([insert_r, 0, box_h - 12]) {
            difference() {
                union() {
                    cylinder(d = insert_col_d, h = 10); 
                    translate([0, 0, -insert_col_d/2])
                        cylinder(d1 = 0, d2 = insert_col_d, h = insert_col_d/2);
                }
                translate([0, 0, 10 - 5.5]) 
                    cylinder(d = insert_d, h = 6.5);
            }
        }
    }

    // 3. PLOTS DE FIXATION MOTEUR SG90 (Forme en "D" avec dégagement)
    translate([0, 0, base_h]) {
        difference() {
            // Cylindres de base de 6.5mm aux entraxes stricts de 27.7mm
            union() {
                translate([0, 9.45, 0]) cylinder(d = 6.5, h = 5.6);
                translate([0, -18.25, 0]) cylinder(d = 6.5, h = 5.6);
            }
            // Trous pour les vis autotaraudeuses du servo
            translate([0, 9.45, 1]) cylinder(d = 1.8, h = 6);
            translate([0, -18.25, 1]) cylinder(d = 1.8, h = 6);
            
            // LE BLOC DE DEGAGEMENT : Coupe nette du côté intérieur des plots
            // pour laisser passer le rectangle massif de 22.8mm du moteur
            translate([-10, -15.7, -1]) cube([20, 22.8, 10]);
        }
    }

    // 4. BATTERIE 18650 (CLIPS INTÉRIEURS)
    translate([0, -24.5, base_h]) 
    difference() {
        union() {
            translate([-28, 0, 0]) cube([8, 10, 16]);
            translate([ 20, 0, 0]) cube([8, 10, 16]);
        }
        translate([-35, 0, 9.25]) 
            rotate([0, 90, 0]) 
            cylinder(d = 19.0, h = 70);
        translate([-35, 7.5, 14])
            rotate([25, 0, 0])
            cube([70, 10, 10]);
    }

    // 5. GLISSIÈRES PCB (Écart intérieur 71.5mm)
    translate([-35.75 - 2, 18, base_h]) {
        difference() { 
            cube([4, 6, 25]); 
            translate([2, 1.8, -1]) cube([3, 2.4, 27]); 
        }
    }
    translate([35.75 - 2, 18, base_h]) {
        difference() { 
            cube([4, 6, 25]); 
            translate([-1, 1.8, -1]) cube([3, 2.4, 27]); 
        }
    }

    // 6. SUPPORT CAPTEUR SCD41
    translate([0, 42, base_h]) { 
        for (x = [-9, 9]) {
            translate([x, 0, 0])
            difference() {
                cylinder(d = 5.5, h = 10);
                translate([0, 0, 2]) cylinder(d = 1.8, h = 9);
            }
        }
    }

    // 7. MODULE TP4056
    translate([-box_d/2 + wall, -9.5, base_h])
    difference() {
        cube([19, 19, 5]);
        translate([-1, 0.9, 1.5]) cube([21, 17.2, 5]); 
    }
}

// =============================================================================
// MODULE : COUVERCLE
// =============================================================================
module lid() {
    difference() {
        union() {
            hull() {
                translate([0, 0, 1]) cylinder(d = box_d, h = lid_h - 1);
                cylinder(d = box_d - 2, h = lid_h);
            }
            translate([0, 0, lid_h])
                cylinder(d = box_d - 2 * wall - 0.6, h = 2.0); 
        }

        for (a = [45, 135, 225, 315]) {
            rotate([0, 0, a])
            translate([insert_r, 0, -1]) {
                cylinder(d = 3.4, h = lid_h + 4);
                translate([0, 0, 0])
                    cylinder(d1 = 6.4, d2 = 3.4, h = 2.4); 
            }
        }
        
        // Accroche murale
        translate([0, 16, -1]) {
            cylinder(d = 8, h = lid_h + 4); 
            translate([-2, 0, 0]) cube([4, 8, lid_h + 4]); 
            translate([0, 8, 0]) cylinder(d = 4.2, h = lid_h + 4); 
        }
    }
}