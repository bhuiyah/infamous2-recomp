/* src/if2_job_images.h -- the SPU image number of each lifted job module, looked up by the module's name.
 *
 * tools/lift_jobs.sh numbers the job modules 200.. in the order of their names, and a name is the SHA-1 of the
 * module's bytes (jobmod_<sha1[:12]>_e<entry>). A copy whose modules differ (a European EBOOT) has other names,
 * so the numbers shift. tools/spu_postlift.py writes generated/spu_jobs/if2_job_images.h from
 * jobmods_register.c: one IF2_IMG_<name> per module this copy has. A native rewrite made from one module uses its
 * define; without it (a copy without that exact module, or a lift older than the header) the rewrite stays off
 * (IF2_IMG_MISSING) instead of replacing a function of another module that happens to sit at the same address.
 */
#ifndef IF2_JOB_IMAGES_H
#define IF2_JOB_IMAGES_H
#if defined(__has_include)
#  if __has_include("../generated/spu_jobs/if2_job_images.h")
#    include "../generated/spu_jobs/if2_job_images.h"
#  endif
#endif
#define IF2_IMG_MISSING (-1)
#endif
