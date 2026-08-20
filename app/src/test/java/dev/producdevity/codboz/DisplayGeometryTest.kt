package dev.producdevity.codboz

import org.junit.Assert.assertEquals
import org.junit.Test

class DisplayGeometryTest {
    @Test
    fun choosesClosestSupportedGameAspectRatio() {
        assertEquals(SurfaceSize(854, 480), DisplayGeometry.guestSurface(1920, 1080))
        assertEquals(SurfaceSize(854, 480), DisplayGeometry.guestSurface(2400, 1080))
        assertEquals(SurfaceSize(854, 480), DisplayGeometry.guestSurface(1216, 2688))
        assertEquals(SurfaceSize(640, 480), DisplayGeometry.guestSurface(2048, 1536))
        assertEquals(SurfaceSize(854, 480), DisplayGeometry.guestSurface(3000, 1000))
        assertEquals(SurfaceSize(640, 480), DisplayGeometry.guestSurface(1000, 1000))
    }

    @Test
    fun switchesAtTheMidpointBetweenSupportedModes() {
        assertEquals(SurfaceSize(640, 480), DisplayGeometry.guestSurface(1493, 960))
        assertEquals(SurfaceSize(854, 480), DisplayGeometry.guestSurface(1496, 960))
    }

    @Test
    fun mapsPhysicalCoordinatesIntoGuestSurface() {
        assertEquals(0, DisplayGeometry.mapCoordinate(-10f, 1921, 854))
        assertEquals(427, DisplayGeometry.mapCoordinate(960.5f, 1921, 854))
        assertEquals(853, DisplayGeometry.mapCoordinate(1921f, 1921, 854))

        assertEquals(0, DisplayGeometry.mapCoordinate(-10f, 1080, 480))
        assertEquals(240, DisplayGeometry.mapCoordinate(540f, 1080, 480))
        assertEquals(479, DisplayGeometry.mapCoordinate(1080f, 1080, 480))
    }

    @Test
    fun fitsSupportedSurfaceWithoutCroppingOrStretching() {
        assertEquals(SurfaceSize(1921, 1080), DisplayGeometry.fitInside(2400, 1080, 854, 480))
        assertEquals(SurfaceSize(1920, 1079), DisplayGeometry.fitInside(1920, 1080, 854, 480))
        assertEquals(SurfaceSize(1440, 1080), DisplayGeometry.fitInside(2048, 1080, 640, 480))
        assertEquals(SurfaceSize(1080, 607), DisplayGeometry.fitInside(1080, 2400, 854, 480))
    }

    @Test
    fun mapsPointersAcrossFullView() {
        assertEquals(
            GuestPointerPosition(35, 240, true),
            DisplayGeometry.mapPointer(100f, 540f, 2400, 1080, 854, 480),
        )
        assertEquals(
            GuestPointerPosition(427, 240, true),
            DisplayGeometry.mapPointer(1200f, 540f, 2400, 1080, 854, 480),
        )
        assertEquals(
            GuestPointerPosition(818, 240, true),
            DisplayGeometry.mapPointer(2300f, 540f, 2400, 1080, 854, 480),
        )
        assertEquals(
            GuestPointerPosition(0, 479, false),
            DisplayGeometry.mapPointer(-1f, 1080f, 2400, 1080, 854, 480),
        )
    }
}
