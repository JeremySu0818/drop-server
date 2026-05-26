export type EncryptedFilePayload = {
  fileIv: string;
  fileCiphertext: string;
  metaIv: string;
  metaCiphertext: string;
};

export type ValidatedEncryptedPayload = {
  key: string;
  files: EncryptedFilePayload[];
};

export type ValidationError = {
  error: string;
};

export type LookupResult = {
  key: string;
};
