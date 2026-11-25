/* ParsePatternNoCase / MatchPatternNoCase */
#include "t.h"

static const char *pats[] = { "#?.info", "~(#?.info)", "a?c", "(foo|bar)#?",
	"[a-c]#?", "#a", "x%y", "'#?", "#?", "abc", 0 };
static const char *names[] = { "Disk.info", "abc", "ABC", "foobar", "barn",
	"aaa", "", "xy", "#?", "c.info", 0 };

int main(u32 arglen, char *args)
{
	char buf[64];
	int i, j;

	for (i = 0; pats[i]; i++) {
		s32 w = call(DOSBase, LVOParsePatternNoCase, "d1d2d3", pats[i],
			     buf, sizeof(buf));
		printf("%-12s wild=%ld:", pats[i], w);
		for (j = 0; names[j]; j++)
			if (call(DOSBase, LVOMatchPatternNoCase, "d1d2", buf,
				 names[j]))
				printf(" \"%s\"", names[j]);
		printf("\n");
	}
	return 0;
}
