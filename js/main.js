document.addEventListener('DOMContentLoaded', () => {
  const btnFs   = document.getElementById('btnFullscreen');
  const card    = document.querySelector('.stream-card');
  const btnBuzz = document.getElementById('btnBuzz');

  console.log('✅ main.js 로드 완료');

  // 전체화면 토글 (버튼 클릭)
  btnFs.addEventListener('click', () => {
    const isFs = card.classList.contains('fullscreen-mode');

    if (!isFs) {
      // → Enter fullscreen
      card.classList.add('fullscreen-mode');
      console.log('▶️ fullscreen-mode 추가됨');

      // Fullscreen API (지원 시)
      if      (card.requestFullscreen)        card.requestFullscreen();
      else if (card.webkitRequestFullscreen)  card.webkitRequestFullscreen();

      // 화면 어디든 클릭 시 exit 처리
      const exitHandler = (e) => {
        // 풀스크린에서 빠져나갈 때만 처리
        if (card.classList.contains('fullscreen-mode')) {
          card.classList.remove('fullscreen-mode');
          console.log('🔙 fullscreen-mode 제거됨 (anywhere click)');

          if      (document.exitFullscreen)       document.exitFullscreen();
          else if (document.webkitExitFullscreen) document.webkitExitFullscreen();
        }
        document.removeEventListener('click', exitHandler);
      };
      // 기존 클릭 이벤트가 끝난 뒤 핸들러가 등록되도록 setTimeout
      setTimeout(() => {
        document.addEventListener('click', exitHandler);
      }, 0);

    } else {
      // → 직접 버튼으로 exit
      card.classList.remove('fullscreen-mode');
      console.log('🔙 fullscreen-mode 제거됨 (button click)');

      if      (document.exitFullscreen)       document.exitFullscreen();
      else if (document.webkitExitFullscreen) document.webkitExitFullscreen();
    }
  });

  // 부저 버튼
  btnBuzz.addEventListener('click', () => {
    console.log('👉 Buzz 버튼 클릭!');
    btnBuzz.disabled = true;

    fetch('/buzz', { method: 'POST' })
      .then(res => {
        console.log('🔄 /buzz 응답 상태:', res.status);
        if (!res.ok) throw new Error('Network response was not ok');

        // 애니메이션 트리거
        btnBuzz.classList.remove('btn-buzz-effect');
        void btnBuzz.offsetWidth;
        btnBuzz.classList.add('btn-buzz-effect');

        setTimeout(() => {
          btnBuzz.classList.remove('btn-buzz-effect');
          btnBuzz.disabled = false;
        }, 600);

        return res.text();
      })
      .catch(err => {
        console.error('❌ 버저 요청 실패:', err);
        btnBuzz.disabled = false;
      });
  });
});
