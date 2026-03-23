/* iogfx_stubs.s — minimal stub for FakeIrisXEFramebuffer link
 * Only defines __antimain and __realmain which are kext entry points
 * that the static linker requires but kmutil provides at load time.
 * ALL IOFramebuffer/IOAccelerator symbols must remain UNDEFINED so
 * kmutil resolves them from IOGraphicsFamily in the kernelcache. */
.data
.globl __antimain
__antimain:
  .quad 0
.globl __realmain
__realmain:
  .quad 0

  .quad 0
  .quad 0
  .quad 0
