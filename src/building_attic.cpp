// 3D World - Building/House Attic Logic
// by Frank Gennari 06/10/2022

#include "3DWorld.h"
#include "function_registry.h"
#include "buildings.h"

colorRGBA get_light_color_temp(float t);
unsigned get_face_mask(unsigned dim, bool dir);
unsigned get_skip_mask_for_xy(bool dim);
colorRGBA apply_light_color(room_object_t const &o, colorRGBA const &c);
void add_boxes_to_space(room_object_t const &c, vect_room_object_t &objects, cube_t const &bounds, vect_cube_t &cubes, rand_gen_t &rgen,
	unsigned num_boxes, float xy_scale, float hmin, float hmax, bool allow_crates, unsigned flags); // from building_room_obj_expand
void try_add_lamp(cube_t const &place_area, float floor_spacing, unsigned room_id, unsigned flags, float light_amt,
	vect_cube_t &cubes, vect_room_object_t &objects, rand_gen_t &rgen);
bool gen_furnace_cand(cube_t const &place_area, float floor_spacing, bool near_wall, rand_gen_t &rgen, cube_t &furnace, bool &dim, bool &dir);


bool building_t::point_under_attic_roof(point const &pos, vector3d *const cnorm) const {
	if (!get_attic_part().contains_pt_xy(pos)) return 0;

	for (auto const &tq : roof_tquads) {
		if (!is_attic_roof(tq, 1)) continue; // type_roof_only=1
		if (!point_in_polygon_2d(pos.x, pos.y, tq.pts, tq.npts, 0, 1)) continue; // check 2D XY point containment
		vector3d const normal(tq.get_norm());
		if (normal.z == 0.0) continue; // skip vertical sides
		if (cnorm) {*cnorm = -normal;} // we're looking at the underside of the roof, so reverse the normal; set whether or not we're inside the attic
		if (dot_product_ptv(normal, pos, tq.pts[0]) < 0.0) return 1;
	}
	return 0;
}
bool building_t::point_in_attic(point const &pos, vector3d *const cnorm) const {
	if (!has_attic() || pos.z < interior->attic_access.z2() || pos.z > interior_z2) return 0; // test attic floor zval
	return point_under_attic_roof(pos, cnorm);
}
bool building_t::cube_in_attic(cube_t const &c) const {
	if (!has_attic() || c.z2() < interior->attic_access.z2() || c.z1() > interior_z2) return 0; // test attic floor zval
	// test the 4 top corners of the cube
	float const z2(c.z2() + 2.5*get_attic_beam_depth()); // account for attic beam depth, which reduces the ceiling height / increases our effective cube height (approximate)
	return (point_under_attic_roof(point(c.x1(), c.y1(), z2)) || point_under_attic_roof(point(c.x1(), c.y2(), z2)) ||
		    point_under_attic_roof(point(c.x2(), c.y2(), z2)) || point_under_attic_roof(point(c.x2(), c.y1(), z2)));
}

void building_t::get_attic_roof_tquads(vector<tquad_with_ix_t> &tquads) const {
	tquads.clear();
	if (!has_attic()) return;

	for (auto const &tq : roof_tquads) {
		if (is_attic_roof(tq, 1)) {tquads.push_back(tq);} // type_roof_only=1
	}
}

bool building_t::has_L_shaped_roof_area() const {
	if (real_num_parts == 1) return 0; // not L-shaped
	cube_t const &A(parts[0]), &B(parts[1]);
	if (A.z2() != B.z2())    return 0; // not at same level
	if (roof_dims == 2)      return 0; // parallel roof
	if (roof_dims == 1)      return 1; // perpendicular roof
	// secondary part's roof is oriented in long dim; if this is the dim adjacent to the primary part,
	// then the two attic areas are connected forming in L-shape; otherwise, there will be two parallel roof peaks with a valley in between
	bool const adj_x(A.x1() == B.x2() || A.x2() == B.x1()), adj_y(A.y1() == B.y2() || A.y2() == B.y1());
	assert(adj_x != adj_y); // must be adjacent in exactly one dim
	return (B.get_sz_dim(!adj_y) < B.get_sz_dim(adj_y));
}

bool building_t::add_attic_access_door(cube_t const &ceiling, unsigned part_ix, unsigned num_floors, unsigned rooms_start, rand_gen_t &rgen) {
	// roof tquads don't intersect correct on the interior for L-shaped house attics, so skip the attic in this case, for now
	//if (has_L_shaped_roof_area()) return 0;
	float const floor_spacing(get_window_vspace());
	cube_t const &part(parts[part_ix]);
	if (min(part.dx(), part.dy()) < 2.75*floor_spacing) return 0; // must be large enough
	// add a ceiling cutout for attic access
	float const half_len(0.24*floor_spacing), half_wid(0.16*floor_spacing);
	room_t best_room;
	float best_area(0.0);
	bool in_hallway(0);

	for (unsigned r = rooms_start; r < interior->rooms.size(); ++r) {
		room_t const &room(interior->rooms[r]);
		if (room.part_id != part_ix) continue;
		if (room.has_stairs_on_floor(num_floors-1)) continue; // skip room with stairs
		if (max(room.dx(), room.dy()) < 2.5*half_len || min(room.dx(), room.dy()) < 2.5*half_wid) continue; // too small
		if (room.is_hallway) {best_room = room; in_hallway = 1; break;} // hallway is always preferred
		// should we reject this room if there's not enough head clearance above it in the attic?
		float const area(room.dx()*room.dy());
		if (area > best_area) {best_room = room; best_area = area;} // choose room with the largest area
	}
	if (best_room.is_all_zeros()) return 0;
	bool const long_dim(best_room.dx() < best_room.dy());
	cube_t valid_area(best_room);
	valid_area.expand_in_dim( long_dim, -1.2*half_len); // add sufficient clearance
	valid_area.expand_in_dim(!long_dim, -1.2*half_wid); // add sufficient clearance
	if (!valid_area.is_strictly_normalized()) return 0; // not enough space for the door (shouldn't be the case)
	rand_gen_t rgen2(rgen); // deep copy to avoid disrupting rgen state
	point access_pos;

	if (in_hallway) {
		access_pos = best_room.get_cube_center();
		access_pos[ long_dim] += (rgen2.rand_bool() ? -1.0 : 1.0)*0.1*best_room.get_sz_dim( long_dim); // place off center to avoid blocking center light
		access_pos[!long_dim] += (rgen2.rand_bool() ? -1.0 : 1.0)*0.2*best_room.get_sz_dim(!long_dim); // place off center to allow player to walk past
	}
	else {
		cube_t const &part(get_part_for_room(best_room));
		// if the room spans the entire part, make the attic access in the center so that the stairs have proper clearance
		bool const span_x(best_room.x1() == part.x1() && best_room.x2() == part.x2()), span_y(best_room.y1() == part.y1() && best_room.y2() == part.y2());
		bool const xd(best_room.xc() < part.xc()), yd(best_room.yc() < part.yc()); // closer to the center of the part to maximize head space
		access_pos.x = (span_x ? best_room.xc() : (0.7*best_room.d[0][xd] + 0.3*best_room.d[0][!xd]));
		access_pos.y = (span_y ? best_room.yc() : (0.7*best_room.d[1][yd] + 0.3*best_room.d[1][!yd]));
	}
	valid_area.clamp_pt_xy(access_pos);
	interior->attic_access.set_from_point(access_pos);
	interior->attic_access.expand_in_dim( long_dim, half_len); // long dim
	interior->attic_access.expand_in_dim(!long_dim, half_wid); // short dim
	set_cube_zvals(interior->attic_access, ceiling.z1(), ceiling.z2()); // same zvals as ceiling
	bool const dir(best_room.get_center_dim(long_dim) < interior->attic_access.get_center_dim(long_dim));
	interior->attic_access.ix = 2*long_dim + dir;
	return 1;
}

cube_t building_t::get_attic_access_door_avoid() const {
	assert(has_attic());
	float const floor_spacing(get_window_vspace());
	cube_with_ix_t avoid(interior->attic_access);
	bool const dim(avoid.ix >> 1), dir(avoid.ix & 1);
	avoid.expand_by_xy(0.25*floor_spacing);
	avoid.d[dim][dir] += (dir ? 1.0 : -1.0)*0.5*floor_spacing; // more spacing in front where the ladder is
	avoid.z2() += 0.5*floor_spacing; // make it taller
	return avoid;
}

void find_roofline_beam_span(cube_t &beam, float roof_z2, point const pts[4], bool dim) {
	swap(beam.d[!dim][0], beam.d[!dim][1]); // start denormalized

	for (unsigned n = 0; n < 4; ++n) { // find the span of the top of the roofline
		if (pts[n].z != roof_z2) continue; // point not at peak of roof
		min_eq(beam.d[!dim][0], pts[n][!dim]);
		max_eq(beam.d[!dim][1], pts[n][!dim]);
	}
}
void create_attic_posts(building_t const &b, cube_t const &beam, bool dim, cube_t posts[2]) {
	assert(beam.is_strictly_normalized());
	cube_t const avoid(b.get_attic_access_door_avoid());

	for (unsigned d = 0; d < 2; ++d) {
		cube_t post(beam);
		set_cube_zvals(post, b.interior->attic_access.z2(), beam.z1()); // extends from attic floor to bottom of beam
		post.d[!dim][!d] = post.d[!dim][d] + (d ? -1.0 : 1.0)*beam.dz();
		assert(post.is_strictly_normalized());
		if (!post.intersects_xy(avoid)) {posts[d] = post;} // skip if too close to attic access door
	} // for d
}

void building_t::add_attic_objects(rand_gen_t rgen) {
	unsigned const obj_flags(RO_FLAG_INTERIOR | RO_FLAG_IN_ATTIC);
	vect_room_object_t &objs(interior->room_geom->objs);
	// add attic access door
	cube_with_ix_t adoor(interior->attic_access);
	assert(adoor.is_strictly_normalized());
	adoor.expand_in_dim(2, -0.2*adoor.dz()); // shrink in z
	int const room_id(get_room_containing_pt(point(adoor.xc(), adoor.yc(), adoor.z1()-get_floor_thickness()))); // should we cache this during floorplanning?
	assert(room_id >= 0); // must be found
	room_t const &room(get_room(room_id));
	bool const ddim(adoor.ix >> 1), ddir(adoor.ix & 1);
	unsigned const acc_flags(room.is_hallway ? RO_FLAG_IN_HALLWAY : 0), attic_door_ix(objs.size());
	float const light_amt(1.0); // always set to 1.0 here, since indir is special cased for attics
	// is light_amount=1.0 correct? since this door can be viewed from both inside and outside the attic, a single number doesn't really work anyway
	objs.emplace_back(adoor, TYPE_ATTIC_DOOR, room_id, ddim, ddir, acc_flags, light_amt, SHAPE_CUBE); // Note: player collides with open attic door
	cube_t const avoid(get_attic_access_door_avoid());
	vect_cube_t avoid_cubes;
	avoid_cubes.push_back(avoid);

	// add light(s)
	cube_t const part(get_part_for_room(room)); // Note: assumes attic is a single part
	bool const long_dim(part.dx() < part.dy());
	float const floor_spacing(get_window_vspace()), beam_depth(get_attic_beam_depth());
	float const sep_dist(part.get_sz_dim(long_dim) - part.get_sz_dim(!long_dim)), attic_height(interior_z2 - adoor.z2()), light_radius(0.03*attic_height);
	point const light_center(part.xc(), part.yc(), (interior_z2 - 1.2*light_radius - beam_depth)); // center of the part near the ceiling
	cube_t light;
	point light_pos[2] = {light_center, light_center}; // start centered
	unsigned num_lights(1);

	if (sep_dist > 0.25*attic_height) { // consider adding two lights
		float const move_dist(0.5*sep_dist - light_radius - beam_depth); // allow extra space for vertical beams
		bool valid(1);

		for (unsigned d = 0; d < 2; ++d) {
			point test_pt(light_center); // spread apart/up an extra radius so that light doesn't partially clip through roof
			test_pt.z += light_radius;
			test_pt[ long_dim] += (d ? -1.0 : 1.0)*(move_dist + light_radius);
			test_pt[!long_dim] += 0.01*sep_dist; // move a tiny bit to the side to avoid incorrect results for queries lying exactly between two roof tquads
			if (!point_in_attic(test_pt)) {valid = 0; break;} // light is outside attic; must be due to hipped roof
		}
		if (valid) {
			light_pos[0][long_dim] -= move_dist;
			light_pos[1][long_dim] += move_dist;
			num_lights = 2;
		}
	}
	for (unsigned n = 0; n < num_lights; ++n) {
		light.set_from_sphere(light_pos[n], light_radius);
		// start off lit for now; maybe should start off and auto turn on when the player enters the attic?
		unsigned const light_flags(RO_FLAG_LIT | RO_FLAG_EMISSIVE | RO_FLAG_NOCOLL | obj_flags);
		objs.emplace_back(light, TYPE_LIGHT, room_id, 0, 0, light_flags, light_amt, SHAPE_SPHERE, get_light_color_temp(0.45)); // yellow-shite
	}
	if (has_chimney == 1) { // interior chimney; not drawn when player is in the attic because it's part of the exterior geometry
		cube_t chimney(get_chimney());
		max_eq(chimney.z1(), adoor.z2());
		min_eq(chimney.z2(), interior_z2); // clip to attic interior range
		assert(chimney.z1() < chimney.z2());
		chimney.expand_by_xy(-0.05*min(chimney.dx(), chimney.dy())); // shrink to make it inside the exterior chimney so that it doesn't show through when outside the attic

		if (!chimney.intersects(avoid)) { // don't block attic access door (probably won't/can't happen)
			objs.emplace_back(chimney, TYPE_CHIMNEY, room_id, 0, 0, obj_flags, light_amt);
			avoid_cubes.push_back(chimney);
		}
	}
	// add posts as colliders; somewhat of a duplicate of the code in building_room_geom_t::add_attic_woodwork()
	for (tquad_with_ix_t const &tq : roof_tquads) {
		if (tq.npts == 3 || !is_attic_roof(tq, 1)) continue; // not a roof tquad; type_roof_only=1
		vector3d const normal(tq.get_norm()); // points outside of the attic
		bool const dim(fabs(normal.x) < fabs(normal.y)), dir(normal[dim] > 0.0); // dim this tquad is facing; beams run in the other dim
		if (dir == 1) continue; // only need to add for one side due to symmetry
		float const beam_width(0.5*beam_depth);
		cube_t const bcube(tq.get_bcube());
		cube_t beam(bcube); // set the z1 base and exterior edge d[dim][dir]
		beam.z1() = beam.z2() - beam_depth; // approximate
		set_wall_width(beam, bcube.d[dim][!dir], 0.5*beam_width, dim); // inside/middle edge
		find_roofline_beam_span(beam, bcube.z2(), tq.pts, dim);
		if (beam.d[!dim][0] == bcube.d[!dim][0]) continue; // not a hipped roof
		if (beam.get_sz_dim(!dim) <= beam_depth) continue; // if it's long enough
		cube_t posts[2];
		create_attic_posts(*this, beam, dim, posts);

		for (unsigned d = 0; d < 2; ++d) {
			if (posts[d].is_all_zeros()) continue;
			objs.emplace_back(posts[d], TYPE_COLLIDER, room_id, dim, 0, (RO_FLAG_INVIS | obj_flags), 1.0);
			avoid_cubes.push_back(posts[d]);
			avoid_cubes.back().expand_by_xy(beam_width); // add extra spacing
		}
	} // for i
	cube_t place_area(part);
	place_area.z1() = place_area.z2() = interior->attic_access.z2(); // bottom of attic floor
	place_area.expand_by_xy(-0.75*floor_spacing); // keep away from corners; just a guess; applies to boxes and furnace

	if (!has_basement()) { // add furnace if not already added in the basement
		for (unsigned n = 0; n < 100; ++n) { // 100 tries
			cube_t furnace;
			bool dim(0), dir(0);
			if (!gen_furnace_cand(place_area, floor_spacing, 0, rgen, furnace, dim, dir)) break; // near_wall=0
			if (has_bcube_int(furnace, avoid_cubes) || !cube_in_attic(furnace)) continue;
			unsigned const flags((is_house ? RO_FLAG_IS_HOUSE : 0) | RO_FLAG_INTERIOR);
			interior->room_geom->objs.emplace_back(furnace, TYPE_FURNACE, room_id, dim, dir, flags, light_amt);
			avoid_cubes.push_back(furnace);
			break; // success/done
		} // for n
	}
	// add boxes; currently not stacked - should they be?
	unsigned const num_boxes(rgen.rand() % 25); // 0-24
	float const box_sz(0.18*floor_spacing);
	add_boxes_to_space(objs[attic_door_ix], objs, place_area, avoid_cubes, rgen, num_boxes, box_sz, 0.5*box_sz, 1.5*box_sz, 1, obj_flags); // allow_crates=1

	// add lamps
	unsigned const num_lamps(rgen.rand() % 3); // 0-2

	for (unsigned n = 0; n < num_lamps; ++n) {
		try_add_lamp(place_area, floor_spacing, room_id, obj_flags, light_amt, avoid_cubes, objs, rgen);
	}

	// TODO: TYPE_RUG, TYPE_CHAIR, TYPE_NIGHTSTAND, TYPE_PAINTCAN, TYPE_LG_BALL, TYPE_BOOK, TYPE_BOTTLE, TYPE_PAPER, TYPE_PIPE
}

cube_t get_attic_access_door_cube(room_object_t const &c, bool inc_ladder) {
	if (!c.is_open()) return c;
	float const len(c.get_sz_dim(c.dim)), thickness(c.dz()), delta(len - thickness);
	cube_t door(c);
	door.z1() -= delta; // open downward
	door.d[c.dim][!c.dir] -= (c.dir ? -1.0 : 1.0)*delta; // shorten to expose the opening
	if (inc_ladder) {door.union_with_cube(get_ladder_bcube_from_open_attic_door(c, door));}
	return door;
}
cube_t get_ladder_bcube_from_open_attic_door(room_object_t const &c, cube_t const &door) {
	float const door_len(c.get_sz_dim(c.dim)), door_width(c.get_sz_dim(!c.dim)), door_inside_edge(door.d[c.dim][!c.dir]);
	cube_t ladder(door); // sets ladder step depth
	ladder.expand_in_dim(!c.dim, -0.05*door_width); // a bit narrower
	ladder.d[c.dim][ c.dir] = door_inside_edge; // flush with open side of door
	ladder.d[c.dim][!c.dir] = door_inside_edge + (c.dir ? -1.0 : 1.0)*2.0*c.dz();
	ladder.z1() = door.z2() - 0.95*(door_len/0.44); // matches door length calculation used in floorplanning step
	return ladder;
}

void building_room_geom_t::add_attic_door(room_object_t const &c, float tscale) {
	rgeom_mat_t &wood_mat(get_wood_material(tscale, 1, 0, 1)); // shadows + small
	colorRGBA const color(apply_light_color(c, c.color));

	if (c.is_open()) {
		unsigned const qv_start1(wood_mat.quad_verts.size());
		cube_t const door(get_attic_access_door_cube(c));
		wood_mat.add_cube_to_verts(door, color, door.get_llc(), 0); // all sides
		// rotate 10 degrees
		point rot_pt;
		rot_pt[ c.dim] = door.d[c.dim][!c.dir]; // door inside edge
		rot_pt[!c.dim] = c.get_center_dim(!c.dim); // doesn't matter?
		rot_pt.z       = door.z2(); // top of door
		vector3d const rot_axis(c.dim ? -plus_x : plus_y);
		float const rot_angle((c.dir ? -1.0 : 1.0)*10.0*TO_RADIANS);
		rotate_verts(wood_mat.quad_verts, rot_axis, rot_angle, rot_pt, qv_start1);
		// draw the ladder
		colorRGBA const ladder_color(apply_light_color(c, LT_BROWN)); // slightly darker
		rgeom_mat_t &ladder_mat(get_wood_material(2.0*tscale, 1, 0, 1)); // shadows + small; larger tscale
		unsigned const qv_start2(ladder_mat.quad_verts.size());
		cube_t const ladder(get_ladder_bcube_from_open_attic_door(c, door));
		float const ladder_width(ladder.get_sz_dim(!c.dim));
		float const side_width_factor = 0.05; // relative to door_width

		for (unsigned n = 0; n < 2; ++n) { // sides
			cube_t side(ladder);
			side.d[!c.dim][!n] -= (n ? -1.0 : 1.0)*(1.0 - side_width_factor)*ladder_width;
			ladder_mat.add_cube_to_verts(side, ladder_color, side.get_llc(), EF_Z1, 1); // skip bottom, swap_tex_st=1
		}
		// draw the steps
		unsigned const num_steps = 10;
		float const step_spacing(ladder.dz()/(num_steps+1)), step_thickness(0.1*step_spacing);
		cube_t step(ladder);
		step.expand_in_dim(!c.dim, -side_width_factor*ladder_width);

		for (unsigned n = 0; n < num_steps; ++n) { // steps
			step.z1() = ladder.z1() + (n+1)*step_spacing;
			step.z2() = step  .z1() + step_thickness;
			ladder_mat.add_cube_to_verts(step, ladder_color, step.get_llc(), get_skip_mask_for_xy(!c.dim), 1); // skip sides, swap_tex_st=1
		}
		rotate_verts(ladder_mat.quad_verts, rot_axis, rot_angle, rot_pt, qv_start2);
	}
	else { // draw only the top and bottom faces of the door
		wood_mat.add_cube_to_verts(c, color, c.get_llc(), ~EF_Z12); // shadows + small, top and bottom only
	}
}

bool building_t::is_attic_roof(tquad_with_ix_t const &tq, bool type_roof_only) const {
	if (!has_attic()) return 0;
	if (tq.type != tquad_with_ix_t::TYPE_ROOF && (type_roof_only || tq.type != tquad_with_ix_t::TYPE_WALL)) return 0;
	cube_t const tq_bcube(tq.get_bcube());
	if (tq_bcube.z1() < interior->attic_access.z1()) return 0; // not the top section that has the attic (porch roof, lower floor roof)
	return get_attic_part().contains_pt_xy_inclusive(tq_bcube.get_cube_center()); // check for correct part
}

struct edge_t {
	point p[2];
	edge_t() {}
	edge_t(point const &A, point const &B, bool cmp_dim) {
		p[0] = A; p[1] = B;
		if (B[cmp_dim] < A[cmp_dim]) {swap(p[0], p[1]);} // make A less in cmp_dim
	}
};

void building_room_geom_t::add_attic_woodwork(building_t const &b, float tscale) {
	if (!b.has_attic()) return;
	cube_with_ix_t const &adoor(b.interior->attic_access);
	get_wood_material(tscale, 0, 0, 2); // ensure unshadowed material
	rgeom_mat_t &wood_mat   (get_wood_material(tscale, 1, 0, 2)); // shadows + detail
	rgeom_mat_t &wood_mat_us(get_wood_material(tscale, 0, 0, 2)); // no shadows + detail
	float const floor_spacing(b.get_window_vspace()), delta_z(0.1*b.get_floor_thickness()); // matches value in get_all_drawn_verts()

	// Note: there may be a chimney in the attic, but for now we ignore it
	for (auto i = b.roof_tquads.begin(); i != b.roof_tquads.end(); ++i) {
		if (!b.is_attic_roof(*i, 0)) continue; // type_roof_only=0
		bool const is_roof(i->type == tquad_with_ix_t::TYPE_ROOF); // roof tquad; not wall triangle
		// draw beams along inside of roof; start with a vertical cube and rotate to match roof angle
		tquad_with_ix_t tq(*i);
		for (unsigned n = 0; n < tq.npts; ++n) {tq.pts[n].z -= delta_z;} // shift down slightly
		cube_t const bcube(tq.get_bcube());
		vector3d const normal(tq.get_norm()); // points outside of the attic
		bool const dim(fabs(normal.x) < fabs(normal.y)), dir(normal[dim] > 0.0); // dim this tquad is facing; beams run in the other dim
		float const base_width(bcube.get_sz_dim(!dim)), run_len(bcube.get_sz_dim(dim)), height(bcube.dz()), height_scale(1.0/fabs(normal[dim]));
		float const beam_width(0.04*floor_spacing), beam_hwidth(0.5*beam_width), beam_depth(2.0*beam_width);
		float const epsilon(0.02*beam_hwidth), beam_edge_gap(beam_hwidth + epsilon), dir_sign(dir ? -1.0 : 1.0);
		unsigned const num_beams(max(2, round_fp(3.0f*base_width/floor_spacing)));
		float const beam_spacing((base_width - 2.0f*beam_edge_gap)/(num_beams - 1));
		// shift slightly for opposing roof sides to prevent Z-fighting on center beam
		float const beam_pos_start(bcube.d[!dim][0] + beam_edge_gap + dir_sign*0.5*epsilon);
		unsigned const qv_start(wood_mat.quad_verts.size());
		cube_t beam(bcube); // set the z1 base and exterior edge d[dim][dir]
		if (is_roof) {beam.z1() += beam_depth*run_len/height;} // shift up to avoid clipping through the ceiling of the room below
		// determine segments for our non-base edges
		edge_t edges[3]; // non-base edge segments: start plus: 1 for rectangle, 2 for triangle, 3 for trapezoid
		unsigned num_edges(0);

		for (unsigned n = 0; n < tq.npts; ++n) {
			point const &A(tq.pts[n]), &B(tq.pts[(n+1)%tq.npts]);
			if (A.z == bcube.z1() && B.z == bcube.z1()) continue; // base edge, skip
			if (A[!dim] == B[!dim]) continue; // non-angled edge, skip
			edges[num_edges++] = edge_t(A, B, !dim);
		}
		assert(num_edges > 0 && num_edges <= 3);
		float const beam_shorten((is_roof ? 2.0 : 1.0)*beam_hwidth*height/(0.5*base_width)); // large for sloped roof to account for width of beams between tquads

		// add vertical beams, which will be rotated to follow the slope of the roof
		for (unsigned n = 0; n < num_beams; ++n) {
			float const roof_pos(beam_pos_start + n*beam_spacing);
			set_wall_width(beam, roof_pos, beam_hwidth, !dim);
			beam.d[dim][!dir] = beam.d[dim][dir] + dir_sign*beam_depth;
			bool found(0);

			for (unsigned e = 0; e < num_edges; ++e) {
				edge_t const &E(edges[e]);
				if (roof_pos < E.p[0][!dim] || roof_pos >= E.p[1][!dim]) continue; // beam not contained in this edge
				if (E.p[0].z == E.p[1].z) {beam.z2() = E.p[0].z;} // horizontal edge
				else {beam.z2() = E.p[0].z + ((roof_pos - E.p[0][!dim])/(E.p[1][!dim] - E.p[0][!dim]))*(E.p[1].z - E.p[0].z);} // interpolate zval
				beam.z2() += (height_scale - 1.0)*(beam.z2() - bcube.z1()); // rescale to account for length post-rotate
				beam.z2() -= beam_shorten; // shorten to avoid clipping through the roof at the top
				assert(!found); // break instead?
				found = 1;
			} // for e
			assert(found);
			if (beam.dz() < 4.0f*beam_depth) continue; // too short, skip
			assert(beam.is_strictly_normalized());
			// skip top, bottom and face against the roof (top may be partially visible when rotated)
			wood_mat.add_cube_to_verts(beam, WHITE, beam.get_llc(), (~get_face_mask(dim, dir) | EF_Z12));
		} // for n
		if (!is_roof) continue; // below is for sloped roof tquads only
		// rotate to match slope of roof
		point rot_pt; // point where roof meets attic floor
		rot_pt[ dim] = bcube.d[dim][dir];
		rot_pt[!dim] = bcube.get_center_dim(dim); // doesn't matter?
		rot_pt.z     = bcube.z1(); // floor
		vector3d const rot_axis(dim ? -plus_x : plus_y);
		float const rot_angle((dir ? 1.0 : -1.0)*atan2(run_len, height));
		rotate_verts(wood_mat.quad_verts, rot_axis, rot_angle, rot_pt, qv_start);

		if (num_edges == 3) { // trapezoid case: add diag beam along both angled edges; dim is long dim
			for (unsigned e = 0; e < num_edges; ++e) {
				edge_t const &E(edges[e]);
				if (E.p[0].z == E.p[1].z) continue; // not an angled edge
				bool const low_ix(E.p[1].z == bcube.z1());
				point const &lo(E.p[low_ix]), &hi(E.p[!low_ix]);
				vector3d const edge_delta(hi - lo);
				float const edge_len(edge_delta.mag());
				vector3d const edge_dir(edge_delta/edge_len);
				beam.set_from_point(lo);
				beam.z1() += beam_depth*run_len/height; // avoid clipping through the floor below
				beam.z2() += edge_len; // will be correct after rotation
				beam.expand_in_dim(!dim, beam_hwidth);
				beam.d[dim][!dir] = beam.d[dim][dir] + dir_sign*beam_depth;
				unsigned const qv_start_angled(wood_mat.quad_verts.size());
				wood_mat.add_cube_to_verts(beam, WHITE, beam.get_llc(), (~get_face_mask(dim, dir) | EF_Z12));
				// rotate into place
				vector3d const axis(cross_product(edge_dir, plus_z));
				float const angle(get_angle(plus_z, edge_dir));
				rotate_verts(wood_mat.quad_verts, axis, angle, lo, qv_start_angled);
				// rotate around edge_dir so that bottom surface is aligned with the average normal of the two meeting roof tquads; always 45 degrees
				rotate_verts(wood_mat.quad_verts, edge_dir*((e>>1) ? 1.0 : -1.0), 0.25*PI, lo, qv_start_angled);
				float const shift_down_val(beam_hwidth*height/run_len);
				for (auto v = wood_mat.quad_verts.begin() + qv_start_angled; v != wood_mat.quad_verts.end(); ++v) {v->v.z -= shift_down_val;}
			} // for e
		}
		if (tq.npts == 4 && dir == 0) {
			// add beam along the roofline for this quad; dim is long dim
			float const centerline(bcube.d[dim][!dir]); // inside/middle edge
			beam = bcube;
			beam.z2() -= beam_hwidth*height/run_len; // shift to just touching the roof at the top
			beam.z1()  = beam.z2() - beam_depth;
			set_wall_width(beam, centerline, beam_hwidth, dim);
			if (num_edges == 3) {find_roofline_beam_span(beam, bcube.z2(), tq.pts, dim);} // trapezoid case (optimization)
			assert(beam.is_strictly_normalized());
			beam.expand_in_dim(!dim, -epsilon); // prevent Z-fighting
			
			if (beam.get_sz_dim(!dim) > beam_depth) { // if it's long enough
				wood_mat_us.add_cube_to_verts(beam, WHITE, beam.get_llc(), EF_Z2); // skip top; shadows not needed
				
				if (num_edges == 3) { // trapezoid: add vertical posts at each end if there's space
					cube_t posts[2];
					create_attic_posts(b, beam, dim, posts);
				
					for (unsigned d = 0; d < 2; ++d) {
						if (!posts[d].is_all_zeros()) {wood_mat.add_cube_to_verts(posts[d], WHITE, posts[d].get_llc(), EF_Z12);} // skip top and bottom
					}
				}
			}
			if (num_edges == 1) { // tilted rectangle (not trapezoid)
				// add horizontal beams connecting each vertical beam to form an A-frame; make them unshadowed because shadows look bad when too close to the light
				beam.z2() -= 3.0*beam_depth; // below roofline beam
				beam.z1()  = beam.z2() - 0.8*beam_depth; // slightly smaller
				float const beam_hlen(((bcube.z2() - beam.z2())/bcube.dz())*run_len); // width of roof tquad at top of beam
				set_wall_width(beam, centerline, beam_hlen, dim);

				for (unsigned n = 1; n+1 < num_beams; ++n) { // same loop as above, but skip the ends
					float const roof_pos(beam_pos_start + n*beam_spacing);
					set_wall_width(beam, roof_pos, 0.9*beam_hwidth, !dim); // slightly thinner to avoid Z-fighting
					wood_mat_us.add_cube_to_verts(beam, WHITE, beam.get_llc(), get_skip_mask_for_xy(dim));
				}
			}
		}
	} // for i
}

void building_room_geom_t::add_chimney(room_object_t const &c, tid_nm_pair_t const &tex) { // inside attic
	tid_nm_pair_t tex2(tex);
	tex2.shadowed = 1;
	tex2.tscale_x *= 4.0; tex2.tscale_y *= 4.0;
	rgeom_mat_t &mat(get_material(tex2, 1, 0, 2));
	mat.add_cube_to_verts(c, c.color, c.get_llc(), (EF_Z12 | EF_Y12), 1); // X sides, swap_tex_st=1
	mat.add_cube_to_verts(c, c.color, c.get_llc(), (EF_Z12 | EF_X12), 0); // Y sides, swap_tex_st=0
}
