// =============================================================================
// PIED DE BUREAU POUR BOITIER BIRDIE 100MM - DESIGN ELEGANT
// =============================================================================

$fn = 100;

// --- PARAMETRES ---
box_z_center = 150; 
peg_z = box_z_center + 24; 

stand();

module stand() {
    union() {
        // 1. BASE (Socle large pour la stabilité)
        hull() {
            translate([0, 45, 0]) cylinder(d=95, h=5);
            translate([0, -15, 0]) cylinder(d=75, h=5); 
        }

        // 2 & 3. BRAS CENTRAL ET FACADE
        hull() {
            translate([0, 25, 4]) cylinder(d=40, h=1);
            translate([0, 15, 80]) rotate([-10, 0, 0]) cylinder(d=25, h=1);
        }
        hull() {
            translate([0, 15, 80]) rotate([-10, 0, 0]) cylinder(d=25, h=1);
            translate([0, 0, box_z_center - 40]) rotate([90, 0, 0]) cylinder(d=20, h=2);
            translate([0, 0, peg_z]) rotate([90, 0, 0]) cylinder(d=16, h=2);
        }

        // 4. PION D'ACCROCHE (S'insère dans le trou de serrure)
        translate([0, 0, peg_z]) {
            rotate([90, 0, 0]) cylinder(d=4.0, h=3.0);
            translate([0, -3.0, 0]) rotate([90, 0, 0]) cylinder(d1=4.0, d2=7.5, h=1.5);
            translate([0, -4.5, 0]) rotate([90, 0, 0]) cylinder(d=7.5, h=0.5);
        }

        // 5. BERCEAU INFERIEUR (Design "Croissant" élégant)
        translate([0, 0, box_z_center]) {
            difference() {
                // Forme adoucie, évasée et organique
                hull() {
                    // Pointe gauche (bords ronds)
                    translate([-35, -8, -38]) rotate([90, 0, 0]) cylinder(d=12, h=16, center=true);
                    // Pointe droite (bords ronds)
                    translate([ 35, -8, -38]) rotate([90, 0, 0]) cylinder(d=12, h=16, center=true);
                    // Base centrale massive fondue dans le mât
                    translate([  0, -8, -55]) rotate([90, 0, 0]) cylinder(d=20, h=16, center=true);
                }
                // Découpe exacte épousant la rondeur du boitier
                rotate([90, 0, 0]) cylinder(d=101, h=40, center=true);
            }
        }
    }
}