# SmartThings Edge Driver 배포 가이드

## 3단계 배포 명령어
1. 드라이버 패키징:
   ```bash
   smartthings edge:drivers:package ./smartthings-gateway-edge-driver
   ```
2. 채널에 드라이버 등록 (Assign):
   ```bash
   smartthings edge:channels:assign
   ```
3. 허브에 드라이버 설치 (Install):
   ```bash
   smartthings edge:drivers:install
   ```

## 실시간 로그 모니터링
```bash
smartthings edge:drivers:logcat
```