#include <ft2build.h>
#include FT_FREETYPE_H
#include <stdio.h>
int main(void){
  FT_Library lib; FT_Face face; long i;
  if(FT_Init_FreeType(&lib)) return 1;
  if(FT_New_Face(lib, "E:/OneDrive/APP/font2c/fonts/msyh.ttc", 0, &face)) return 2;
  printf("num_faces=%ld\n", face->num_faces);
  FT_Done_Face(face);
  for(i=0;i<10;i++){
    if(FT_New_Face(lib, "E:/OneDrive/APP/font2c/fonts/msyh.ttc", i, &face)==0){
      printf("face %ld: family=%s style=%s\n", i, face->family_name ? face->family_name : "", face->style_name ? face->style_name : "");
      FT_Done_Face(face);
    }
  }
  FT_Done_FreeType(lib);
  return 0;
}
