from pydantic import BaseModel
from typing import Optional

class TokenExchangeRequest(BaseModel):
    code: str

class DownloadRequest(BaseModel):
    url: str
    stream: Optional[str] = "file"
    instance_id: Optional[str] = "default"

class NextRequest(BaseModel):
    client_id: Optional[str] = None
    instance_id: Optional[str] = "default"

class QueueRemoveRequest(BaseModel):
    id: str
    client_id: Optional[str] = None
    instance_id: Optional[str] = "default"
