#pragma once

// [Karto] Find answers from an index that is only valid while its buffer is unwritten; code that changes script array
// elements outside the array natives must note it here.

// The buffer (TArray::Data()) may have changed.
void ArrayIndexNoteData(const void *data);
// The element at this address may have changed (the address may be anywhere, not only in an array).
void ArrayIndexNoteElement(const void *address);
// The buffer is being freed.
void ArrayIndexDropData(const void *data);
// Script arrays were rebuilt from outside the natives (a game was loaded).
void ArrayIndexClearAll();
// A world tic starts.
void ArrayIndexTic();
