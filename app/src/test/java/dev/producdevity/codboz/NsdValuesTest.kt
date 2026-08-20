package dev.producdevity.codboz

import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class NsdValuesTest {
    @Test
    fun acceptsGuestServiceType() {
        assertTrue(NsdValues.SERVICE_TYPE.matches("_PROJECT_KIWI._tcp"))
    }

    @Test
    fun rejectsMalformedServiceTypes() {
        assertFalse(NsdValues.SERVICE_TYPE.matches("PROJECT_KIWI._tcp"))
        assertFalse(NsdValues.SERVICE_TYPE.matches("_PROJECT_KIWI._http"))
        assertFalse(NsdValues.SERVICE_TYPE.matches("_PROJECT_KIWI_._tcp"))
    }
}
