include <BOSL2/std.scad>
include <BOSL2/screws.scad>
include <BOSL2/joiners.scad>

$fs = 0.1;
thick = 2.2;
i_w = 16.1;   // .3 ?
i_l = 16.2;
h = 8.0;  // 8
tab_w = 2.4;
tab_th = 0.4;   // 0.4
tab_over = -0.2;

diff(){
    cuboid([i_w+thick*2,i_l+thick*2,h]);    
    tag("remove") cuboid([i_w,i_l,h+0.01]);  //,chamfer=tab_over, edges=[TOP]); 
}

xcopies(i_w-tab_th) recolor("red") translate([0,0,tab_over/2]) cuboid([tab_th,tab_w,h+tab_over]);
