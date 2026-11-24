// Magmora patch 1 (MIT, see VENDORED.md): independent text entry filter, written from the NSFormatter API.
@implementation IGRAPHICS_FORMATTER

- (instancetype) initWithAllowed: (NSCharacterSet*) allowed limit: (NSUInteger) limit
{
  if ((self = [super init]))
  {
    mAllowed = [allowed copy];
    mLimit = limit;
  }
  return self;
}

- (void) dealloc
{
  [mAllowed release];
  [super dealloc];
}

- (NSString*) stringForObjectValue: (id) value
{
  return [value description];
}

- (BOOL) getObjectValue: (id*) value forString: (NSString*) text errorDescription: (NSString**) error
{
  if (value)
    *value = [[text copy] autorelease];
  return YES;
}

// The selection-aware variant, so that a refused edit leaves both the text and the caret where they were.
- (BOOL) isPartialStringValid: (NSString**) proposed
        proposedSelectedRange: (NSRangePointer) selection
               originalString: (NSString*) original
        originalSelectedRange: (NSRange) originalSelection
             errorDescription: (NSString**) error
{
  NSString* text = *proposed;
  const BOOL fits = !mLimit || text.length <= mLimit;
  const BOOL clean = !mAllowed || [[text stringByTrimmingCharactersInSet: mAllowed] length] == 0;

  if (fits && clean)
    return YES;

  *proposed = original;
  if (selection)
    *selection = originalSelection;
  return NO;
}

@end
