"""The camera of a section page, in plain Python (no bpy).

Every page has a control plane that faces the camera squarely, as the head's panel does. The camera
looks along `forward`, may sit `elevation` millimetres up the plane (along `up`) and uses lens shift so
that the plane still maps linearly to the screen. That is what lets one knob atlas serve every knob:
each control is sheared by its offset from the axis, so that it renders as if seen straight on.

Screen values are points on the 1280 x 576 render, origin top left.
"""
import math

RENDER_W, RENDER_H = 1280, 576
LENS_MM, SENSOR_MM = 85.0, 36.0


def _norm(v):
    n = math.sqrt(sum(c * c for c in v))
    return tuple(c / n for c in v)


def _cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


class View:
    def __init__(self, centre, forward, up, pt_per_mm, elevation=0.0):
        self.centre = tuple(centre)              # world mm, lands on the middle of the render
        self.forward = _norm(forward)
        self.up = _norm(up)
        self.right = _cross(self.forward, self.up)
        self.ppm = pt_per_mm
        self.elevation = elevation
        self.view_w = RENDER_W / pt_per_mm
        self.dist = self.view_w * LENS_MM / SENSOR_MM

    def plane(self, x, y):
        """(a, b): millimetres along right and up from the centre, on the control plane."""
        return (x - RENDER_W / 2) / self.ppm, (RENDER_H / 2 - y) / self.ppm

    def world(self, x, y, depth=0.0):
        """The point `depth` mm behind the control plane (along forward) that lands on (x, y)."""
        k = (self.dist + depth) / self.dist
        a, b = self.plane(x, y)
        a, b = a * k, self.elevation + (b - self.elevation) * k
        return tuple(self.centre[i] + self.right[i] * a + self.up[i] * b + self.forward[i] * depth
                     for i in range(3))

    def screen(self, p):
        """Inverse of world() for any point."""
        rel = [p[i] - self.centre[i] for i in range(3)]
        a = sum(rel[i] * self.right[i] for i in range(3))
        b = sum(rel[i] * self.up[i] for i in range(3))
        d = sum(rel[i] * self.forward[i] for i in range(3))
        k = self.dist / (self.dist + d)
        return RENDER_W / 2 + a * k * self.ppm, RENDER_H / 2 - (self.elevation + (b - self.elevation) * k) * self.ppm

    def scale_at(self, depth):
        return self.ppm * self.dist / (self.dist + depth)

    def camera_position(self):
        return tuple(self.centre[i] - self.forward[i] * self.dist + self.up[i] * self.elevation for i in range(3))

    def shear(self, x, y):
        """Local-frame shear (x, y per unit of local z toward the camera) that cancels perspective at (x, y)."""
        a, b = self.plane(x, y)
        return -a / self.dist, -(b - self.elevation) / self.dist
