/* Offline ir3 compiler probe for A740 (chip 0x43050a01).
 * Mirrors turnip's NIR preparation closely enough to compile SPIR-V to A7xx
 * machine code without a GPU, a driver or Vulkan.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "compiler/glsl_types.h"
#include "compiler/nir/nir.h"
#include "compiler/nir/nir_builder.h"
#include "compiler/spirv/nir_spirv.h"
#include "freedreno/common/freedreno_dev_info.h"
#include "freedreno/ir3/ir3_compiler.h"
#include "freedreno/ir3/ir3_nir.h"
#include "freedreno/ir3/ir3_shader.h"
#include "util/u_math.h"

static void *read_file(const char *path, size_t *size)
{
   FILE *f = fopen(path, "rb");
   if (!f)
      return NULL;
   fseek(f, 0, SEEK_END);
   long n = ftell(f);
   fseek(f, 0, SEEK_SET);
   void *data = malloc((size_t)n);
   if (!data || fread(data, 1, (size_t)n, f) != (size_t)n) {
      fclose(f);
      free(data);
      return NULL;
   }
   fclose(f);
   *size = (size_t)n;
   return data;
}

int main(int argc, char **argv)
{
   if (argc < 3) {
      fprintf(stderr, "usage: %s <vert|frag> <shader.spv> [out.bin]\n", argv[0]);
      return 2;
   }

   mesa_shader_stage stage;
   if (!strcmp(argv[1], "vert"))
      stage = MESA_SHADER_VERTEX;
   else if (!strcmp(argv[1], "frag"))
      stage = MESA_SHADER_FRAGMENT;
   else {
      fprintf(stderr, "stage must be vert or frag\n");
      return 2;
   }

   size_t size = 0;
   void *data = read_file(argv[2], &size);
   if (!data || size % 4) {
      fprintf(stderr, "cannot read %s or bad size\n", argv[2]);
      return 2;
   }

   struct fd_dev_id dev_id = { .chip_id = 0x43050a01ull };
   const struct fd_dev_info *info = fd_dev_info_raw(&dev_id);
   if (!info) {
      fprintf(stderr, "no fd_dev_info for 0x43050a01\n");
      return 2;
   }

   struct ir3_compiler_options copts = {
      .push_ubo_with_preamble = false,
      .disable_cache = true,
   };
   struct ir3_compiler *compiler = ir3_compiler_create(NULL, &dev_id, info, &copts);
   if (!compiler) {
      fprintf(stderr, "ir3_compiler_create failed\n");
      return 2;
   }

   glsl_type_singleton_init_or_ref();

   const nir_shader_compiler_options *nir_options = ir3_get_compiler_options(compiler);

   struct spirv_to_nir_options sopts = { .create_library = false };
   nir_shader *nir = spirv_to_nir((const uint32_t *)data, size / 4, NULL, stage,
                                  "main", &sopts, nir_options);
   if (!nir) {
      fprintf(stderr, "spirv_to_nir failed\n");
      return 2;
   }

   if (stage == MESA_SHADER_FRAGMENT) {
      bool progress = false;
      NIR_PASS(progress, nir, nir_lower_terminate_to_demote);
      if (progress) {
         NIR_PASS(_, nir, nir_lower_halt_to_return);
         NIR_PASS(_, nir, nir_lower_returns);
      }
   }

   nir->info.num_ubos = 0;
   nir->info.num_ssbos = 0;

   NIR_PASS(_, nir, nir_lower_global_vars_to_local);
   NIR_PASS(_, nir, nir_opt_find_array_copies);
   NIR_PASS(_, nir, nir_opt_copy_prop_vars);
   NIR_PASS(_, nir, nir_opt_dce);

   if (stage == MESA_SHADER_FRAGMENT) {
      const nir_lower_sysvals_to_varyings_options sysval_options = {
         .primitive_id = true,
      };
      NIR_PASS(_, nir, nir_lower_sysvals_to_varyings, &sysval_options);
   }

   nir_shader_gather_info(nir, nir_shader_get_entrypoint(nir));

   NIR_PASS(_, nir, nir_split_var_copies);
   NIR_PASS(_, nir, nir_lower_var_copies);
   NIR_PASS(_, nir, nir_lower_mediump_vars,
            nir_var_function_temp | nir_var_shader_temp | nir_var_mem_shared);
   NIR_PASS(_, nir, nir_opt_copy_prop_vars);
   NIR_PASS(_, nir, nir_opt_combine_stores, nir_var_all);
   NIR_PASS(_, nir, nir_lower_system_values);
   NIR_PASS(_, nir, nir_lower_is_helper_invocation);

   NIR_PASS(_, nir, nir_split_per_member_structs);
   ir3_nir_lower_io_vars_to_temporaries(nir);
   ir3_nir_lower_io(nir);
   NIR_PASS(_, nir, nir_recompute_io_bases,
            nir_var_shader_in | nir_var_shader_out);

   struct ir3_optimize_options optimize_options = { 0 };
   ir3_optimize_loop(compiler, &optimize_options, nir);

   nir_opt_peephole_select_options peephole_select_options = {
      .limit = 0,
      .discard_ok = true,
   };
   NIR_PASS(_, nir, nir_opt_peephole_select, &peephole_select_options);

   nir_shader_gather_info(nir, nir_shader_get_entrypoint(nir));

   struct ir3_shader_options shopts = {
      .api_wavesize = IR3_SINGLE_OR_DOUBLE,
      .real_wavesize = IR3_SINGLE_OR_DOUBLE,
      .push_consts_type = IR3_PUSH_CONSTS_NONE,
   };

   struct ir3_shader *shader = ir3_shader_from_nir(compiler, nir, &shopts);
   if (!shader) {
      fprintf(stderr, "ir3_shader_from_nir failed\n");
      return 2;
   }

   if (getenv("IR3_PROBE_DEBUG"))
      nir_print_shader(nir, stderr);

   struct ir3_shader_key key = { 0 };
   ir3_key_clear_unused(&key, shader);

   struct ir3_shader_variant *v =
      ir3_shader_get_variant(shader, &key, false, false, NULL, NULL);
   if (!v) {
      fprintf(stderr, "ir3_shader_get_variant failed\n");
      return 2;
   }

   if (getenv("IR3_PROBE_DEBUG"))
      ir3_shader_disasm(v, v->bin, stderr);

   printf("SHADER stage=%s gen=%u instrs=%u consts=%u sizedwords=%u\n", argv[1],
          compiler->gen, v->instrlen, v->constlen, v->info.sizedwords);
   unsigned nwords = v->info.sizedwords;
   printf("WORDS %u\n", nwords);
   for (unsigned i = 0; i < nwords; i++)
      printf("%08x%c", v->bin[i], (i % 8 == 7 || i + 1 == nwords) ? '\n' : ' ');

   if (argc >= 4) {
      FILE *out = fopen(argv[3], "wb");
      if (out) {
         fwrite(v->bin, 4, nwords, out);
         fclose(out);
      }
   }

   ir3_shader_destroy(shader);
   ir3_compiler_destroy(compiler);
   return 0;
}
