// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#import "flutter/shell/platform/darwin/macos/framework/Source/FlutterTextureRegistrar.h"

#import "flutter/shell/platform/darwin/macos/framework/Source/FlutterEngine_Internal.h"

#import <os/lock.h>

@implementation FlutterTextureRegistrar {
  __weak id<FlutterTextureRegistrarDelegate> _delegate;

  __weak FlutterEngine* _flutterEngine;

  // A mapping of textureID to internal FlutterExternalTexture wrapper.
  NSMutableDictionary<NSNumber*, FlutterExternalTexture*>* _textures;

  // Guards `_textures`. register/unregister mutate it on the platform thread
  // while getTextureWithID: reads it on the raster thread during external-
  // texture resolution; NSMutableDictionary is not thread-safe. Leaf lock —
  // held only across the dictionary op, never across an engine callout.
  os_unfair_lock _texturesLock;
}

- (instancetype)initWithDelegate:(id<FlutterTextureRegistrarDelegate>)delegate
                          engine:(FlutterEngine*)engine {
  if (self = [super init]) {
    _delegate = delegate;
    _flutterEngine = engine;
    _textures = [[NSMutableDictionary alloc] init];
    _texturesLock = OS_UNFAIR_LOCK_INIT;
  }
  return self;
}

- (int64_t)registerTexture:(id<FlutterTexture>)texture {
  FlutterExternalTexture* externalTexture = [_delegate onRegisterTexture:texture];
  int64_t textureID = [externalTexture textureID];
  BOOL success = [_flutterEngine registerTextureWithID:textureID];
  if (success) {
    os_unfair_lock_lock(&_texturesLock);
    _textures[@(textureID)] = externalTexture;
    os_unfair_lock_unlock(&_texturesLock);
    return textureID;
  } else {
    NSLog(@"Unable to register the texture with id: %lld.", textureID);
    return 0;
  }
}

- (void)textureFrameAvailable:(int64_t)textureID {
  BOOL success = [_flutterEngine markTextureFrameAvailable:textureID];
  if (!success) {
    NSLog(@"Unable to mark texture with id %lld as available.", textureID);
  }
}

- (void)unregisterTexture:(int64_t)textureID {
  bool success = [_flutterEngine unregisterTextureWithID:textureID];
  if (success) {
    // Freeing the wrapper synchronously on this (platform) thread is a
    // use-after-free: ResolveTexture on the raster thread borrows a raw pointer
    // into the wrapper's std::vector storage, and a resolve for an already-
    // submitted frame may still be in flight. -unregisterTextureWithID: posts
    // the engine-side registry removal onto the raster task runner (a single
    // FIFO queue), so deferring the wrapper's release onto the same runner
    // frees it strictly after that removal and any queued resolve. The block's
    // strong capture keeps it alive until then.
    os_unfair_lock_lock(&_texturesLock);
    FlutterExternalTexture* wrapper = _textures[@(textureID)];
    [_textures removeObjectForKey:@(textureID)];
    os_unfair_lock_unlock(&_texturesLock);
    [_flutterEngine releaseOnRenderThread:wrapper];
  } else {
    NSLog(@"Unable to unregister texture with id: %lld.", textureID);
  }
}

- (FlutterExternalTexture*)getTextureWithID:(int64_t)textureID {
  os_unfair_lock_lock(&_texturesLock);
  // ARC retains the result into the caller's strong local, so it outlives the
  // unlock even if a concurrent unregister removes it from the dictionary.
  FlutterExternalTexture* texture = _textures[@(textureID)];
  os_unfair_lock_unlock(&_texturesLock);
  return texture;
}

@end
