// Magmora patch 1 (MIT, see VENDORED.md): independent text entry filter, written from the NSFormatter API.
@interface IGRAPHICS_FORMATTER : NSFormatter
{
  NSCharacterSet* mAllowed;
  NSUInteger mLimit; // 0: no limit
}

- (instancetype) initWithAllowed: (NSCharacterSet*) allowed limit: (NSUInteger) limit;

@end
